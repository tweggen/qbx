# app/objects/wave — CONTRACT

Purpose: the sample object. SPlainWave (resident WAV via twWavInput, page-
cached preview), its inline renderer, the shared waveform drawing helper,
and add-sample / remove-sample actions.

Public headers: app/objects/wave/*.h

Depends on (engine): tw/core, tw/graph, tw/sources (+pages/schedule via
model base). App edges: per tools/check_layering.py.

Invariants:
1. getDuration() is project-rate frames from twWavInput::getLength()
   (viewAtRate) — a truncated file is clamped to real data at load; do not
   "fix" the duration to match the header.
2. getRandomSource() hands out the PROJECT-RATE view; consumers mint their
   own readers (never share the twWavInput cursor).
3. setWave() registers with the project's extern-file list; the destructor
   deregisters via its OWN project (not the app's current one).
4. Preview is getStraightPreview (sidecar-backed), and its probes fold EVERY
   CHANNEL of the source — the union of the per-channel signed envelopes
   (proposal 36 B8; app/model/CONTRACT.md inv. 9a). fetchPreviewSidecar
   cross-checks the stored header's `channels` against the source's, and
   twAspect::PreviewPeaksVersion is 2, so a v1 file (written under the
   channel-0 rule) orphans rather than being adopted.
   It does NOT go through the aspect page cache any more, and never did in
   practice: the old page branch reinterpret_cast a CapturePageData's FLOAT
   payload to preview_t* (proposal 36 trap 26) and was unreachable, because
   only SCut ever calls scheduleRevalidation so a wave's currentPage_ is
   always null. Removing it was behaviour-preserving; "fixing" the cast would
   not have been, since that page carries no probe geometry to fix it against.
5. fileName_ is ABSOLUTE; only the serializer sees the portable spelling.
   serializeSelfAttributes() runs SFilePathRef::toStored() against the
   project's own path, and instantiateFromDomElement() runs fromStored()
   before linkToFile(). A relative reference that does NOT resolve next to
   the project file falls back to its raw spelling, so linkToFile()'s older
   resolution (the .qxa runner's sample base dir, else the working
   directory) still applies — that is what keeps hand-written fixtures and
   pre-encoding projects loading. As a LAST resort, whatever the spelling
   (relative, "~" or absolute), if the resolved reference does not exist but
   a same-named file sits in the project's OWN directory, that neighbour is
   adopted — recordings are written into the project folder, so a project
   moved/copied as a unit keeps its samples beside the .qxp even when the
   baked-in path names the old location. Self-healing: the next save
   re-encodes the found path project-relative. Only when even that misses is
   the reference used as resolved, so a failure names the file it looked for.
   Gate: sample_recovered_beside_project.qxa.

Self-registration (Phase 5): splainwave.cpp registers "SPlainWave" with
SProjectLoader AND the SProject extern-file factory (WAV loading) from
static initializers.

## SRecordingContent — the GROWING recording (proposal 21 L3b, design D7)

`srecordingcontent.{h,cpp}` + `srecordingrndrinline.{h,cpp}`: an `SObject`
whose duration GROWS while a capture is running, and its inline renderer. It
lives HERE, beside `SPlainWave`, because it is the same kind of thing — audio
content backed by a `twRandomSource` — and because `objects/wave` is the
lowest module that may include `tw/sources` and is visible from `shell` (the
recorder), `objects/cut` (the window) and `timeline` (the arranger).

Invariants:

R1. **It is a VIEW of a `twGrowingCaptureSource`**, `[startFrame, frontier)`,
    not an owner of samples. Several armed tracks show one capture without a
    second copy of the audio, and a punch-in that begins mid-capture starts at
    its own frame.

R2. **`getRootComponent()` is NULL and `isLiveRecording()` is true**, and
    `STrack` therefore keeps the clip OUT of the bus mixers entirely — the same
    routing decision `objects/track` already makes for MIDI clips, for the same
    reason: there is no component to freeze pages from, so inserting it would
    cost a dummy freeze per page per clip AND make
    `twView::getComponent() returned nullptr` fire once per freeze forever.
    What the user HEARS while recording is the live monitor lane (D9's
    "Auto = input while stopped or recording"); what they SEE is this clip.

R3. **`getRandomSource()` IS the growing source, non-null.** That is what
    routes `SCutRendererInline` down its sample-backed branch to
    `getInlineRenderer()` rather than down the container branch, which would
    demand a rendered capture that will never exist.

R4. **The peak ladder is EXTENDED FROM THE FRONTIER, never recomputed**, in
    whole hops only, and only below the frontier that produced the published
    length. A five-minute take rescanned ten times a second is 90 GB of reads a
    minute; folding a partial hop would bake silence into a bucket for frames
    that are about to arrive.

R5. **`getDuration()` answers the PUBLISHED length, not the live frontier.**
    Every consumer that asks — the cut's window, the track's geometry, the
    renderer's time map — must agree with the last `durationChanged` the
    recorder emitted, or the drawn clip and its own waveform disagree by up to
    a tick.

R6. **The bridge thread never touches this object.** It appends to the pages
    and stores a frontier; `publishGrowth()` is called by `SAudioRecorder` on
    the MAIN thread at ~10 Hz and is the only thing that emits anything
    (THREADING.md rule 1; design section 4).

R7. **The frontier rule is drawn HERE, in the recording renderer**, not in
    `SCutRendererInline`: it belongs to the capture, not to the window, so two
    cuts over one capture both show it and no existing clip gains a branch it
    would have to skip.

Known debt: `SRecordingRendererInline::draw()` has NO gate. `screenshot` grabs
the screen's root window (blank offscreen), and the verbs that do gate a paint
build one named widget off screen — there is none for the arranger canvas. The
peak ladder it draws from IS gated (`record_punch`, `previewNonEmpty`).

Also: an `SRecordingContent` has no loader registration, so SAVING a
project mid-take writes an element the loader will not recognise. The object
exists for the length of a take and is replaced by a WAV-backed cut at stop;
refusing the save, or materialising the partial take, is a later decision.

How to test: every qxa case that add-samples; the user-project renders
(loading real WAVs, including truncated ones); sample_path_portable.qxa for
the stored spelling and load_project_render.qxa for the legacy fallback.

Also: record_offset_zero.qxa / record_loop_takes.qxa / record_punch.qxa
(record_punch is the one that asserts a NON-EMPTY preview mid-take).

THE COLLECT SEAM (proposal 39 M1). `swaveformdraw` now has TWO entry points
and exactly ONE probe-producing path:

  collectObjectEnvelope( obj, lk, SEnvelopeWindow, preview_t *out )
  drawObjectWaveform   ( obj, lk, SRenderContext, QColor )   // collect + draw

The collect takes a TIME WINDOW, never an `SRenderContext` — a context holds a
`QPainter&`, so a headless caller (the `assert-envelope` verb, the folder-sum
overlay of proposal 39 M3) could only build one over a scratch `QImage`: a
painter that exists solely to be ignored. `drawObjectWaveform` derives the
window from its context (`envelopeWindowOfContext`) and is otherwise unchanged.
That derivation MOVED to `app/model/sobjectrenderer.h` at M3, beside
`SEnvelopeWindow` itself: the folder-sum overlay derives the same window for a
LANE, and `objects/track` may not include `objects/wave`.

Every renderer that draws a waveform overrides `SObjectRenderer::collectEnvelope`
by routing its EXISTING walk to the collect terminal; the base returns false and
writes nothing, which is the right answer for an object with no waveform.
Gates: `preview_envelope_test` (ctest — the collected probes against the DRAWN
pixels, through the link-start fold, a cut's slip and loop tiling),
`envelope_probe.qxa` (the shipped renderers, end to end) and
`envelope_offset_window.qxa` (the only one whose CLIP and WINDOW both start
somewhere other than 0 — a cut's collect clamps a negative clip-relative
position to 0, so nothing anchored at 0 can tell a correct map from that clamp).

WHAT A DRAWN WAVEFORM DESCRIBES (proposal 39 M2). One rule, and it is the
reason `drawObjectWaveform` no longer looks at `lk.parent()` at all:

> **A drawn waveform describes the audio its object PRODUCES. The lane it is
> drawn on never scales it.**

Until M2 the draw half read the containing track's `volumeDbSnapshot()` and
multiplied every probe by it, so pulling a fader down redrew every clip on that
lane as a thinner waveform and at -40 dB the arrangement visually emptied. Wrong
three independent ways: a waveform is the CONTENT and a fader is the LEVEL, and
this app has a fader widget AND a level meter (proposal 34) for the level; the
multiply landed AFTER the probes were quantised to 8 bits, so a -20 dB clip drew
as a coarse dozen-valued ladder rather than as a quieter version of itself; and
it contradicted what the stored bytes mean — volume is not baked into a preview
(`plan/STATE.md:6576-6580`), so the paint-time multiply was the whole
dependency. The `qBound` to [-127,127] stays: it costs nothing on probes already
in range, and M3's folder-sum overlay accumulates into this same domain, where a
saturated column is the normal case.

The rule is about the CONTAINER, not about content. A container object's OWN
preview is legitimately post-fader — `straightCalcPreviewData()`'s no-random-
source branch reads `getRootComponent()`'s frozen pages, and for a track that
root is `cpRewire_`, downstream of `twGainStage` — which is why
`SObject::setVolume()` still calls `invalidatePreview()`, and why an ASSET
clip keeps the REFERENCED track's fader: that track is the clip's content, not
its container.

Gate: `preview_envelope_test` section 5 — the painted pixels against the
collected probes through a link whose parent object holds -60/-20/-6/0/+6 dB.
It has to be a PIXEL gate, because the collect half never carried the multiply
and everything reading through `collectEnvelope` is blind to it by construction.
Verified to fail before the deletion (at -20 dB a painted column read 2/-2 where
the collect said 20/-20). `preview_volume_independent.qxa` states the same rule
at script level, over `set-track-volume` and `set-track-mute`, and is what pins
it for every LATER consumer of the seam.

Known debt: swaveformdraw is shared by other renderers (cut) — candidate
for a render-support module when slices become real targets.

## A sample that will not load becomes a PLACEHOLDER, never a deletion

`SPlainWave::setMissingWave()` / `isMissing()` / `linkToMissingFile()`
(2026-08-22).

**Invariant: a `<SPlainWave>` whose file cannot be loaded still produces an
object, and every clip built on it survives the load unchanged.**

Before this, `instantiateFromDomElement` returned NULL and the loader —
correctly, given that — dropped the element and cascaded the drop to every
`<SCut>` windowing it, "so the rest of the project can load"
(`sprojectloader.cpp`). The cost was silent and permanent: a project opened on a
machine where one sample happens to be absent came up MINUS those clips, and the
next save wrote that arrangement back. Measured on a real project carried from
Windows to Linux: 3 unreachable samples, **3 waves and 4 clips gone**.

| Rule | Why |
|---|---|
| A placeholder owns a **source-less `twWavInput`** (constructed with an EMPTY file name), not a null component | That ctor spelling loads nothing and warns about nothing, and what comes out is an ordinary component whose every render path already answers "no source" with silence. It is what makes the graph, the capture builder, the scheduler, the freeze path and the RT callback need NO missing-file branch. |
| `getDuration()` reports the **project's recorded `durationSec`**, not the component's | There is no file to measure and `twWavInput::getLength()` answers -1 with no source, so every clip on the absent sample would load with a nonsense extent. The attribute is read in `instantiateFromDomElement`, because `SPlainWave` overrides `hasDuration()`/`getDuration()` out of its component and `SObject::readPreChildrenAttributes`' `duration_` is therefore never consulted for a wave. |
| `serializeSelfAttributes` writes the **spelling the project file used, verbatim** — never `SFilePathRef::toStored` | The reference is unresolvable HERE, so this machine has no standing to rewrite where it points. Re-encoding it would take a project carried from another OS and rewrite its unreachable samples relative to a folder they were never in — breaking it on the machine where they DO exist, silently, one save away. Gate: `missing_sample_reference_verbatim.qxa`, whose fixture is deliberately PROJECT-RELATIVE; the home-relative fixture in `collect_external_media_missing.qxa` re-encodes to the same string and cannot bite this. |
| `relocateTo()` **REFUSES** on a placeholder | There are no bytes to have been copied, so "Collect external media" reports it as skipped rather than quietly re-pointing a reference at a file that is not there either. |
| `enqueueAnalysis()` is deliberately NOT called | No content, and a null content hash is declined by every sidecar job anyway. |
| The placeholder is **registered like any other extern file** (`addExternObject`) | It must appear in the resources list — that is where the user sees the miss — a second clip on the same absent sample must find it, and `~SPlainWave` deregisters by name whether it loaded or not. |

**A placeholder must be routed AROUND `SCut::buildCapture_`** (see
`main/objects/cut/CONTRACT.md` and `SObject::isMissing()`): it has no random
source, so every test there classifies it as CONTAINER-BACKED and the function
renders its whole declared duration into a capture — a full-length buffer of
zeros, built on the UI thread, once per clip. Measured before the early return:
**~17 s of load** on the project above, all of it producing silence.

Gates: `sample_missing_survives.qxa`, `load_missing_sample_placed_survives.qxa`
(both extended with `assert-extern-files count/missing`),
`missing_sample_reference_verbatim.qxa`,
`collect_external_media_missing.qxa`. **Only `assert-extern-files` can bite
here**: a dropped clip and a placeholder are BOTH SILENT, so no audio assertion
anywhere can separate them — which is exactly how the drop shipped under a green
suite for as long as it did.

**NOT gated:** what a placeholder LOOKS like on the arranger (it falls to the
solid-body fill, since `SCut::getPreview` has no capture to build peaks from);
re-linking by pointing the project at the file again through the UI (there is no
"Locate…" dialog — the answer today is to put the file back and re-open);
a placeholder inside a take stack or a fragment; and `setMissingWave` on a file
that EXISTS but whose FORMAT this build cannot decode, which takes the same path
and has no case of its own.
