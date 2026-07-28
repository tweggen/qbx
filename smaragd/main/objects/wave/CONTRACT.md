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
4. Preview goes through the page cache with live fallback
   (getStraightPreview) — never block painting on revalidation.
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
