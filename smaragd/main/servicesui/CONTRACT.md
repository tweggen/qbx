# app/servicesui — CONTRACT

Purpose: dialogs over the engine services — render dialog (range/format/
quality → RenderParams), render progress, recording progress, options
dialog (audio devices, latency info, log settings) — plus the log dock's
model and view (proposal 24).

Public headers: app/servicesui/*.h

Depends on (engine): tw/core, tw/render, tw/record, tw/devices, tw/playback.
App edges: model, shell.

Invariants:
1. Progress dialogs POLL session query methods from the GUI thread — they
   must not register Qt-touching callbacks on session threads
   (THREADING.md rule 1).
2. The render dialog emits absolute start/end seconds; a marked range is
   passed through as-is (the session handles non-zero starts).
3. Device ids chosen in options persist via SSettings and apply on next
   startOutput.
4. SLogModel POLLS the TwLog ring on a timer and holds a cursor. It must not
   be signalled by the sink: producers are engine and worker threads that may
   never touch Qt (rule 1 again, and the reason TwLog is not a QObject).
5. A drain tick is bounded by TIME (`kDrainBudgetMs`), never by a record
   count. The backlog is displayed rather than absorbed in one go — a fixed
   count blocked the GUI thread for 105 ms on a burst.
6. The log view never uses QSortFilterProxyModel and never gives the table a
   variable row height. Both reintroduce per-row cost that scales with the
   ring (see the header comments; `log_dock_scale.qxa` fails if either goes).

How to test: `ctest -R qxa.log_dock_scale` covers the log dock's scale
requirement numerically (300k records, asserts the worst drain tick). The
other dialogs are manual; the headless render action bypasses them by design.

Known debt: render extent enum duplicated between dialog and params. The log
page's "write a rotating log file" setting only takes effect on restart —
starting/stopping the writer mid-session was not worth the complexity.
