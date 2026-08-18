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
2a. The render dialog DISPLAYS the channel count and does not override it
   (proposal 36 B8 decision 3). RenderParams::channels comes from
   SProject::channels(), or 0 = "ask the graph" when there is no project,
   exactly as B5 built it. An override was considered and rejected: reducing
   6 channels to 2 needs channel roles and a fold law, which proposal 36 §8
   names as a non-goal, and the obvious candidate — the device rule's "first
   two channels, the rest dropped" — is a listening compromise that must not
   be applied silently to a delivered file. One authority for the width, and
   a control that cannot lie about what the render will contain.
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
7. The MIDI page (proposal 37 P7b) mirrors the Audio page — same
   build/load/apply triple, same rule that the port list comes from the ACTIVE
   backend rather than a hard-coded table (an Options page showing the
   machine's real MIDI ports while the capture backend is running would be a
   lie). It asks `SApplication::midiOutPump()` for the lists and never mints a
   `MidiOutput` of its own: `CaptureMidiOutput` registers the most recently
   constructed live instance as the one the testkit reads, so a throwaway port
   would unregister the real one when it died.
8. "Create virtual port" is gated on the CAPABILITY the backend reports, never
   on the platform, and never by calling createVirtualPort() to find out —
   that would create one. WinMM has no such concept (tw/devices CONTRACT
   inv. 18); a loopMIDI-style driver shows up as an ordinary device instead.
   Same shape as the MP3 render option's gating.
9. Input ports are LISTED and PERSISTED here (`midi/inputPortIds`), and since
   proposal 21 L2 the page is ACTIVE: applying it OPENS every selected port
   through `SMidiInputHub`. The list is the HUB's, not the out-pump's probe,
   so the COMPUTER KEYBOARD appears in it - it is a real in-process MidiInput
   port (design D9) and it is listed FIRST, because it is the one that exists
   whatever `SMARAGD_MIDI_BACKEND` chose. Deselecting does NOT close a port: a
   track may be armed on it right now, and the Options dialog is not where a
   monitoring session gets torn down.

10. **The Media page (proposal 38 GATE 5b) is a LIVE page, not a
    build/load/apply one — closer to the Plugins page's "Rescan now" than to
    the MIDI page.** Add/Save/Remove/Test connection commit IMMEDIATELY
    through `SApplication::mediaAccounts()` (`SMediaAccountManager`), because
    the state that matters — the account list, the registered
    `SWebDavMediaSource` — lives on that long-lived manager, not on this
    dialog; `apply()`/`accept()` therefore touch nothing extra for this page.
    `describeMediaPage()` follows `describeMidiPage()`'s shape (one line per
    fact, `key=value`, a `  acct[i]=...` row per account) and NEVER prints a
    password value, length or prefix (AC 15) — only
    `set|unset|undecryptable`.

How to test: `ctest -R qxa.log_dock_scale` covers the log dock's scale
requirement numerically (300k records, asserts the worst drain tick). The
other dialogs are manual; the headless render action bypasses them by design.

Known debt: the MIDI page cannot EDIT the `midi/portId/<name>` mapping — it lists the machine's ports, but a project referring to a port name that matches none of them has to be re-pointed by editing smaragd.ini.

Known debt: render extent enum duplicated between dialog and params. The log
page's "write a rotating log file" setting only takes effect on restart —
starting/stopping the writer mid-session was not worth the complexity.

`SOpt` gained `midi/recordMode` (new-take | overdub | replace) and
`midi/recordQuantize` (off | 1/4 | 1/8 | 1/16 | 1/8t | ...) for proposal 21
L4. Both are GLOBAL and per-user, not per track: "how does a recorded pass
combine with what is already there" and "quantise the input" are properties
of how this person works, and every reference DAW puts them in one place
beside the transport. `SMidiRecorder` reads both ONCE at each record start,
so a change made during a take cannot make the commit disagree with the
capture. The defaults are the two that cannot destroy anything: new-take
(the previous performance is still take 0) and off (a performance nobody
asked to have quantised arrives as played). **Known debt: neither has a UI
control yet** - the Options MIDI page does not show them, and a headless
case reaches them through `set-option`.

## Transport polish (proposal 21 L5)

Three per-USER keys and two verbs live here, and they live here for one reason:
this is the module that owns the option table (`SOpt`) and the only one that may
include both it and `SSettings`.

- `SOpt::MetronomeLevel` (linear amplitude of the ACCENTED click, 0..1, default
  0.5), `SOpt::CountInBars` and `SOpt::PreRollBars` (0..8, both default 0). An
  ordinary beat is HALF the accented level, so the accent ratio is exactly 2 and
  a gate can assert the ladder in closed form.
- `set-count-in` / `set-pre-roll` (`STransportBarsAction`) write those last two.
  **NOT undoable**, exactly as `set-option` is not: how many bars this person
  likes before a take is a preference, and putting a preference on the
  arrangement's undo stack would make Ctrl-Z mean two different things. They are
  named verbs rather than two spellings of `set-option` because a named verb can
  refuse a range (8 bars at 60 BPM is half a minute of waiting; past that it is
  a typo) and because `set-option` lives in the TESTKIT while these are
  production behaviour.
- **Whether the metronome is ON is a PROJECT property** (`SProjectProps::
  Metronome`, on the transport bar) and deliberately stays one: it travels with
  the arrangement. Only how loud it is is per-user.

The Options dialog's AUDIO page carries all three, beside the recording offset —
that is where the transport's other timing numbers already are. Changing the
level takes effect through the live plan's SIGNATURE (the 40 ms demand tick
compares it), which is the same arrangement a fader move uses; there is no
signal to wire.

**This module now has an edge to `app/actions`** (`tools/check_layering.py`), for
the two verbs above and nothing else.
