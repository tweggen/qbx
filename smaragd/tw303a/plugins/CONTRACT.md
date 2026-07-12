# tw/plugins — CONTRACT

Purpose: the plugin ABI (twPlugin: prepare/process/reset, params, state
blobs), descriptors, the registry, and the hosting chain components
(twPluginChain, twPluginInsert). twPassThrough is the built-in test plugin
(a bit-crusher).

Public headers: twplugin.h, twplugindescriptor.h, twpluginchain.h,
twplugininsert.h.

Depends on: tw/core, tw/graph. Forbidden: app headers, devices/sinks.

Invariants:
1. Plugin discovery is SYMBOL-referenced (the registry calls
   createPassThroughPlugin() directly) — NOT static-init self-registration,
   so static-library linking is safe here. Keep it that way, or move the
   engine to whole-archive linking first.
2. process() is realtime: no allocation, no locks, no Qt.
3. saveState()/loadState() blobs round-trip through project files — versioned
   and tolerant of unknown trailing data.

How to test: `ctest -R plugins_test` (registry + insert processing) and
qxa.render_sawtooth_with_effects (chain in the signal path); the plugin
browser lists exactly the registry contents.

Known debt: registry hardcodes the built-in plugin (CLAP/external scan is
proposal 08); insert latency compensation not modeled.
