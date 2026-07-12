# app/servicesui — CONTRACT

Purpose: dialogs over the engine services — render dialog (range/format/
quality → RenderParams), render progress, recording progress, options
dialog (audio device selection, latency info).

Public headers: app/servicesui/*.h

Depends on (engine): tw/render, tw/record, tw/devices, tw/playback. App
edges: model, shell.

Invariants:
1. Progress dialogs POLL session query methods from the GUI thread — they
   must not register Qt-touching callbacks on session threads
   (THREADING.md rule 1).
2. The render dialog emits absolute start/end seconds; a marked range is
   passed through as-is (the session handles non-zero starts).
3. Device ids chosen in options persist via SSettings and apply on next
   startOutput.

How to test: manual dialogs; the headless render action bypasses this
module by design.

Known debt: render extent enum duplicated between dialog and params.
