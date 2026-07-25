#!/bin/bash
# ab_warp.sh — proposal 27 M3 A/B time-stretch/pitch quality driver.
#
# For every corpus WAV x transform, renders the placed clip twice — once with
# TW_STRETCH_BACKEND=rubberband (the reference) and once with
# TW_STRETCH_BACKEND=vocoder (the candidate paged vocoder, written concurrently)
# — then scores the pair with warp_ab and appends a Markdown summary table.
#
# The vocoder backend does NOT exist yet: the engine ignores TW_STRETCH_BACKEND
# for now, so both renders currently produce identical Rubber Band output. That
# is the EXPECTED reference-side state and itself validates the plumbing
# end-to-end (all metrics ~perfect on identical pairs). The script tolerates a
# missing or byte-identical candidate and marks those rows clearly, so it keeps
# working unchanged once the vocoder lands.
#
# Deterministic and re-runnable: the corpus is regenerated from fixed seeds, the
# work dir is cleaned each run, renders are byte-exact per machine.
#
# Usage:  ./ab_warp.sh [report.md]     (default: /tmp/ab_report.md)
#
# Must be run from anywhere; sample paths in the generated .qxa resolve relative
# to the .qxa's own directory (SProject::setSampleBaseDir).

set -u

REPORT="${1:-/tmp/ab_report.md}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$SCRIPT_DIR/../build/bin"

# Locate binaries (.exe on Windows/Git Bash).
find_bin() {
    if   [ -x "$BIN_DIR/$1.exe" ]; then echo "$BIN_DIR/$1.exe"
    elif [ -x "$BIN_DIR/$1" ];     then echo "$BIN_DIR/$1"
    else echo ""; fi
}
SMARAGD="$(find_bin smaragd)"
WARP_AB="$(find_bin warp_ab)"

if [ -z "$SMARAGD" ]; then echo "Error: smaragd binary not found in $BIN_DIR"; exit 2; fi
if [ -z "$WARP_AB" ]; then echo "Error: warp_ab binary not found in $BIN_DIR (build target warp_ab)"; exit 2; fi

WORK="${TMPDIR:-/tmp}/ab_warp_work"
rm -rf "$WORK"
mkdir -p "$WORK/corpus"

echo "Generating corpus..."
"$WARP_AB" --gen "$WORK/corpus" >/dev/null || { echo "corpus generation failed"; exit 2; }

# Corpus files (basenames — the .qxa lives beside them so filePath is a basename).
CORPUS=(corpus_saw220.wav corpus_sine440.wav corpus_voice.wav corpus_transients.wav)

# Transforms: "label|stretch|duration|cents"  (empty stretch/duration => no
# resize; empty cents => no pitch). Base clip is 192000 frames (4.0 s @ 48 kHz).
TRANSFORMS=(
    "stretch 2/1|2.0|384000|"
    "stretch 1/2|0.5|96000|"
    "stretch 3/2 + pitch +300c|1.5|288000|300"
    "pitch +1200c|||1200"
    "pitch -700c|||-700"
)

# Extract a key's value from a warp_ab SUMMARY|k=v|... line.
field() { printf '%s' "$1" | tr '|' '\n' | grep "^$2=" | cut -d= -f2-; }

# Emit one .qxa placing $wav on a track with the given transform, render to
# out.wav.  $1=qxa path  $2=wav basename  $3=stretch  $4=duration  $5=cents
write_qxa() {
    local qxa="$1" wav="$2" stretch="$3" duration="$4" cents="$5"
    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<SActionScript version="1" name="ab_warp">'
        echo '  <setup project="new"/>'
        echo '  <actions>'
        echo '    <add-track index="-1"/>'
        echo "    <add-sample trackIndex=\"0\" filePath=\"$wav\" timePos=\"0\"/>"
        if [ -n "$stretch" ]; then
            echo "    <resize-clip clip=\"0,0\" startTime=\"0\" startOffset=\"0\" duration=\"$duration\" loopLength=\"0\" stretch=\"$stretch\"/>"
        fi
        if [ -n "$cents" ]; then
            echo "    <set-pitch clip=\"0,0\" cents=\"$cents\"/>"
        fi
        echo '    <render filename="out.wav" format="wav" quality="10"/>'
        echo '  </actions>'
        echo '  <assertions>'
        echo '    <assert-track-count equals="1"/>'
        echo '  </assertions>'
        echo '</SActionScript>'
    } > "$qxa"
}

# Render $qxa with a given backend into its own output dir; echo the WAV path (or
# empty on failure).  $1=qxa  $2=backend  $3=tag
render_with() {
    local qxa="$1" backend="$2" tag="$3"
    local odir="$WORK/out_$tag"
    rm -rf "$odir"; mkdir -p "$odir"
    TW_STRETCH_BACKEND="$backend" SMARAGD_SIDECAR_DIR=off \
        "$SMARAGD" --test-case "$qxa" --test-output-dir "$odir" >/dev/null 2>&1
    if [ -f "$odir/out.wav" ]; then echo "$odir/out.wav"; else echo ""; fi
}

# ---- report header ----
{
    echo "# A/B time-stretch / pitch quality report"
    echo ""
    echo "- Generated: $(date -u '+%Y-%m-%d %H:%M:%SZ')"
    echo "- Reference backend: \`TW_STRETCH_BACKEND=rubberband\`"
    echo "- Candidate backend: \`TW_STRETCH_BACKEND=vocoder\`"
    echo "- Corpus: deterministic 16-bit PCM stereo 48 kHz, 4.0 s each (warp_ab --gen)"
    echo "- Sidecars disabled (\`SMARAGD_SIDECAR_DIR=off\`)"
    echo ""
    echo "Status legend: **identical** = candidate bytes == reference (vocoder not"
    echo "yet distinct; plumbing validated, all metrics must be ~0) · **A/B** = the"
    echo "two backends diverged, metrics are meaningful · **candidate-missing** ="
    echo "vocoder produced no output · **ref-missing** = reference render failed."
    echo ""
    echo "Metric columns (candidate vs reference): RMSΔ overall %, per-second max"
    echo "RMS dev %, dominant-freq max dev %, transient rise-time ratio mean/max,"
    echo "unpaired onsets ref/cand, warble ΔdB (cand−ref, +flag), spectral-balance"
    echo "max band ΔdB."
} > "$REPORT"

for wav in "${CORPUS[@]}"; do
    echo "Processing $wav ..."
    {
        echo ""
        echo "## $wav"
        echo ""
        echo "| Transform | Status | RMSΔ% | perSec maxdev% | freq maxdev% | rise ratio mean/max | unpaired r/c | warble ΔdB | specbal maxΔdB |"
        echo "|---|---|---|---|---|---|---|---|---|"
    } >> "$REPORT"

    for tspec in "${TRANSFORMS[@]}"; do
        IFS='|' read -r label stretch duration cents <<< "$tspec"
        qxa="$WORK/corpus/ab_${wav%.wav}.qxa"
        write_qxa "$qxa" "$wav" "$stretch" "$duration" "$cents"

        ref_wav="$(render_with "$qxa" rubberband ref)"
        cand_wav="$(render_with "$qxa" vocoder cand)"

        if [ -z "$ref_wav" ]; then
            echo "| $label | ref-missing | - | - | - | - | - | - | - |" >> "$REPORT"
            continue
        fi
        if [ -z "$cand_wav" ]; then
            echo "| $label | candidate-missing | - | - | - | - | - | - | - |" >> "$REPORT"
            continue
        fi

        status="A/B"
        if cmp -s "$ref_wav" "$cand_wav"; then status="identical"; fi

        summary="$("$WARP_AB" "$ref_wav" "$cand_wav" 2>/dev/null | grep '^SUMMARY|')"
        if [ -z "$summary" ]; then
            echo "| $label | $status (score-failed) | - | - | - | - | - | - | - |" >> "$REPORT"
            continue
        fi

        rmsd="$(field "$summary" rms_overall_dpct)"
        psec="$(field "$summary" rms_persec_maxdev_pct)"
        freq="$(field "$summary" freq_maxdev_pct)"
        smean="$(field "$summary" smear_mean)"
        smax="$(field "$summary" smear_max)"
        upr="$(field "$summary" unpaired_ref)"
        upc="$(field "$summary" unpaired_cand)"
        wdb="$(field "$summary" warble_delta_db)"
        wfl="$(field "$summary" warble_flag)"
        sb="$(field "$summary" specbal_max_db)"

        wflag=""; [ "$wfl" = "yes" ] && wflag=" ⚑"
        echo "| $label | $status | ${rmsd} | ${psec} | ${freq} | ${smean}/${smax} | ${upr}/${upc} | ${wdb}${wflag} | ${sb} |" >> "$REPORT"
    done
done

echo ""
echo "Report written to $REPORT"
echo ""
cat "$REPORT"
