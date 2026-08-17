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

Known debt: an `SRecordingContent` has no loader registration, so SAVING a
project mid-take writes an element the loader will not recognise. The object
exists for the length of a take and is replaced by a WAV-backed cut at stop;
refusing the save, or materialising the partial take, is a later decision.

How to test: every qxa case that add-samples; the user-project renders
(loading real WAVs, including truncated ones); sample_path_portable.qxa for
the stored spelling and load_project_render.qxa for the legacy fallback.

Also: record_offset_zero.qxa / record_loop_takes.qxa / record_punch.qxa
(record_punch is the one that asserts a NON-EMPTY preview mid-take).

Known debt: swaveformdraw is shared by other renderers (cut) — candidate
for a render-support module when slices become real targets.
