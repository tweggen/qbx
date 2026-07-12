# tw/mix — CONTRACT

Purpose: arrangement composition — twTrackMix (clips on a timeline, THE
consumer of the clip model), twMixer (bus summing), twRewire (fan-out).

Public headers: twtrackmix.h, twmixer.h, twrewire.h.

Depends on: tw/core, tw/pages, tw/graph. Forbidden: tw/sources (the mixer
sees only components via twView), app headers.

Invariants (normative detail in CLIP_MODEL.md and POSITION_DOMAINS.md):
1. ClipEntry identity is the opaque caller key (the app passes the SLink*);
   NEVER match clips by component pointer.
2. twTrackMix hands clips CLIP-RELATIVE positions; twView's MapPosFn does
   the domain translation.
3. freezePage_nolock clamps mixed child pages to the clip end
   (framesToMix = min(validFrames, clipEnd - mixStart)) — frozen pages carry
   full pages of material.
4. clip.previousPage chains per-clip DSP state across track pages.
5. seekTo_nolock seeks ALL clips (not just nearby ones) so no stale cursors
   survive a jump.

Threading: clips_ mutations (insert/update/remove) are UI-thread under the
component mutex; render paths hold the same mutex during page assembly.

How to test: `ctest -R mix_test` (clip windows, MapPosFn, clamp, key-based
update/remove against a scripted component — mix/tests/);
qxa.render_split_slip_offset and qxa.render_sawtooth_multiple_clips
end-to-end.

Known debt: calcOutputTo allocates buffers per block; per-clip gain/pan not
yet modeled (track-level only).
