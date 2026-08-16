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

How to test: every qxa case that add-samples; the user-project renders
(loading real WAVs, including truncated ones); sample_path_portable.qxa for
the stored spelling and load_project_render.qxa for the legacy fallback.

Known debt: swaveformdraw is shared by other renderers (cut) — candidate
for a render-support module when slices become real targets.
