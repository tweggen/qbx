# tw/dsp — CONTRACT

Purpose: unit generators and filters — plain twComponents with no knowledge
of tracks, files, or devices: twOsc, twSaw, twSimpleSaw, twMoog,
twWhiteNoise, twConstant, twPipe, twTestSeq.

Public headers: one per unit (tw/dsp/twosc.h, ...).

Depends on: tw/core, tw/graph. Forbidden: everything else.

Invariants:
1. Every unit obeys the five component sub-contracts (tw/graph CONTRACT);
   stateful units (twMoog) implement capture/restoreInternalState so pages
   chain seamlessly (FREEZE_PROTOCOL.md).
2. reset() returns bit-identical initial state — offline renders must be
   deterministic.

Threading: units are pulled from audio/render threads; parameter setters are
UI-thread; keep the snapshot/atomic pattern for new parameters.

How to test: render_sawtooth_*.qxa (synth path end-to-end).

Known debt: none tracked beyond graph-level debt (allocation in pull path).
