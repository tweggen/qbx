# Proposal 54 — Cross-platform plugin identity, with graded confidence

Status: proposed. Owner: unassigned. Touches `tw303a/plugins/`,
`main/objects/track/` (SPluginSlot), `main/pluginui/`, `main/shell/`.

Prior art: proposal 08 (plugin hosting; M4 gave us the `createNullPlugin()`
placeholder, the `uid` round-trip and `reloadPlugin()`), proposal 50 (the
nassau-suite SDK, which is what makes tier T1 below cheap for our own plugins).

---

## 1. The report, and what is actually true

A project authored on Windows (`test5.qxp`, 13 `<SPluginSlot>` entries) opens on
macOS with its plugins missing. Measured on the dev Mac, 2026-10-05:

| what the project asks for | format | uid | present on this Mac? |
|---|---|---|---|
| NassauEQ | clap | `com.Nassau.NassauEQ` | no |
| Mangrove ×2 | clap | `com.Nassau.Mangrove` | **yes** |
| Castello Reverb | clap | `com.lucianoiam.castello-reverb` | no |
| Smaragd 303 | tw | `tw.native.303` | yes (built in) |
| Dexed | vst3 | `01EFCDAB…657864` | no |
| NassauZermatt ×2 | vst3 | `0DE7…4E7A6D31` | no |
| NassauAnalogue ×3 | vst3 | `0DE7…4E616E31` | no |
| NassauEQ | vst3 | `0DE7…4E657131` | no |
| Mangrove | vst3 | `0DE7…4D6E6735` | no — **but its CLAP sibling is** |

`~/Library/Audio/Plug-Ins/CLAP/` holds exactly one module
(`MangrovePlugin.clap`); `VST3/` and `Components/` are empty; the only other
entries in `~/.config/Smaragd/plugincache.v2.json` are Apple's stock AUs.

So three different things are being confused under "it does not find them", and
only the second and third are ours to fix:

1. **Genuinely not installed** — 9 of the 13 slots. No identity scheme invents a
   binary that is not on the disk. The honest answer for these is a *legible
   report* (§5), not a match.
2. **Installed, and should already resolve** — the two Mangrove CLAP slots.
   CLAP ids are platform-independent by spec, so `(clap, com.Nassau.Mangrove)`
   matches byte-for-byte. That they come up Missing anyway is a bug: §3.
3. **Installed in a different format** — the Mangrove VST3 slot, whose product
   *is* on this machine, as a `.clap`. Recovering this is what §4 is for, and it
   is the general case: a user who moves between Windows and macOS moves between
   format populations, not just paths.

## 2. Why the uid alone cannot carry this

Each format's uid is stable *across platforms* and useless *across formats*:

- **CLAP** — a reverse-DNS string chosen by the vendor, identical everywhere.
- **VST3** — a 128-bit class id. `vst3UidFromTuid()` already normalizes it to
  COM byte order via `FUID::to4Int()`, so Windows and macOS agree (see the
  comment block at `twvst3module.cc:196`). Stable, and unrelated to the CLAP id
  of the same product.
- **AU** — type/subtype/manufacturer codes, `auUidFromCodes()`. macOS-only by
  construction: an AU slot authored on a Mac can never resolve on Windows, and
  no amount of care changes that.
- **tw** — ours, stable, trivially portable.

`twPluginDescriptor` has one `uid` field and `findByUid( format, uid )` is an
exact lookup. Everything below adds tiers *under* that lookup; T0 stays exactly
what it is today.

## 3. Two bugs to fix before any matching is written

Both are small and both are load-bearing — without them the tiers in §4 would be
papering over a plain defect.

**B1 — nothing re-resolves a slot when the scan finishes.**
`SPluginSlot::resolveEffective()` runs once, from the constructor
(`spluginslot.cpp:48`). `SApplication::init()` ends with `initPluginRegistry()`,
which fires `rescanAsync()` (`sapplication.cpp:594`); the startup project then
loads from `SMainWindow::openMostRecent()`, on the main thread, *while the scan
thread is still running*. Every slot therefore resolves against a registry that
holds only the built-ins, and the only re-resolution path in the codebase is the
manual Reload button on the effect strip (`splugineffectstrip.cpp:791`). This
alone explains the two Mangrove CLAP slots.

Fix: have the project (or `SMainWindow`) connect
`SApplication::pluginScanFinished` and call `reloadPlugin()` on every slot whose
state is `Missing`. The signal is already emitted on the main thread from
`pumpPluginScan()` precisely so a consumer can do this; `reloadPlugin()` already
re-resolves, re-pushes curves, re-applies the state chunk and invalidates
downstream. Disconnect-free and idempotent: a slot that is `Active` is skipped.

**B2 — a foreign absolute module path is used verbatim.**
`resolveModulePath()` (`spluginslot.cpp:110`) returns any absolute path
unchanged, so after a failed uid lookup the fallback descriptor carries
`C:/Program Files/Common Files/VST3/Dexed.vst3` and macOS is asked to load it.
Harmless today (the loader refuses) but it costs a load attempt per slot and it
pollutes the log. Recognize a path that cannot exist on this platform — a
drive-letter or backslash root on non-Windows, a `/Library/…` or `/usr/…` root
on Windows — and clear it rather than pass it down. `descriptor_` is still never
rewritten (proposal 08 M4's rule), only `effective_.path`.

## 4. The tiers

One new entry point on the registry, leaving `findByUid()` alone:

```c++
enum class twPluginMatchTier {
    Exact = 0,      // T0  same format, same uid
    Declared,       // T1  vendor-declared cross-format product id
    Learned,        // T2  equivalence this machine observed, or the user confirmed
    SameProduct,    // T3  vendor + name + I/O, any format
    NameOnly,       // T4  name or bundle stem + I/O, any format
    None,           // T5  nothing
};

struct twPluginMatch {
    twPluginDescriptor  descriptor;      // valid unless tier == None
    twPluginMatchTier   tier = twPluginMatchTier::None;
    bool                restoreState = false;   // see §4.1
    std::string         reason;          // one line, for the tooltip and the log
};

// What a project file can tell us. A superset of today's attributes so the
// resolver can be tested without a project.
struct twPluginRef {
    std::string format, uid, name, vendor, path, productId, version;
    twPluginIoLayout io;
    bool isInstrument = false;
};

twPluginMatch twPluginRegistry::resolve( const twPluginRef & ) const;
std::vector<twPluginMatch>
twPluginRegistry::candidates( const twPluginRef &, int maxN ) const;  // for the dialog
```

**T0 — Exact.** `(format, uid)`. Today's behaviour, unchanged, and the only tier
that needs no policy. Confidence: certain.

**T1 — Declared product id.** A single reverse-DNS string the plugin reports
identically from every wrapper it ships — `com.nassau.mangrove` from the CLAP,
the VST3 and the AU build of one product. Three places to read it from:

- CLAP: a `clap_plugin_descriptor.features` entry (`tw:product=…`), or simply
  the CLAP id itself, which is already in that shape.
- VST3: a custom `IPluginFactory` class attribute, or `subCategories`.
- AU: a key in the bundle's `Info.plist`.

For our own plugins this is nearly free: proposal 50 put all four Nassau
products on one SDK, so the SDK emits the string once and all three wrappers
inherit it. For third-party plugins it needs vendor cooperation we will not get,
so T1 is backed by a **shipped alias table** — `plugins/data/pluginaliases.json`,
`{"vst3:0DE7…4D6E6735": "com.Nassau.Mangrove", …}` — seeded with our own
products and whatever the community contributes, loaded at registry
construction, overridable per user. A table lookup is a declaration by someone
who knew; treat it as T1 and say *who* in `reason`.
Confidence: high. Scales to the plugins we control, which is where it matters.

**T2 — Learned equivalence.** Two sources, both local and both free:

- *Observed*: when a scan sees two modules in different formats with the same
  vendor, name, version and I/O, record the pairing in the plugin cache as an
  equivalence set. A user who once had Windows VST3 and macOS CLAP of one
  product both scanned (on their respective machines, with the cache synced, or
  simply on one machine where both are installed) gets the mapping for free.
- *User-confirmed*: when the user fixes a Missing slot by hand — picking a
  replacement in the browser, or accepting a T3/T4 suggestion — **write that
  choice down**, keyed by the unresolved `(format, uid)`, in a per-user
  `pluginaliases.user.json`. Apply it silently forever after.

This is the highest-value tier per line of code, because it needs neither vendor
cooperation nor heuristics, and because the user only ever answers a given
question once. Confidence: high (confirmed) / medium-high (observed).

**T3 — Same product, any format.** Normalized `vendor` + `name` + I/O
compatibility + equal `isInstrument`. Normalization: casefold, strip
non-alphanumerics, strip the vendor prefix from the name when it repeats it
(`NassauEQ` under vendor `Nassau` → `eq`), strip format noise (`Plugin`, `VST`,
`VST3`, `AU`, `CLAP`, `x64`). I/O compatible means `nIn`/`nOut` equal, or the
candidate is wider and the extra channels can be zero-fed. This is the tier that
would pair the project's `NassauEQ` VST3 slot with a `NassauEQ.clap`, and
Mangrove's VST3 slot with the installed `MangrovePlugin.clap`.
Confidence: medium. Two vendors can ship same-named products; one vendor can
rename. Default policy: **suggest, do not apply** (see §4.2).

**T4 — Name or bundle stem only.** The same normalization with the vendor
dropped, plus a bundle-stem comparison (`MangrovePlugin.vst3` ↔
`MangrovePlugin.clap`) taken from `path`'s basename, still gated on I/O
compatibility. Confidence: low. Never applied without the user; exists so the
Missing-plugins dialog has something ranked to offer instead of an empty list.

**T5 — None.** Exactly today's behaviour: `createNullPlugin()`, the slot stays
inert, `descriptor_` and the state chunk round-trip on save so the project is
still correct on the machine that does have the plugin. This is a *feature*, and
no tier below T0 may ever overwrite `uid=` in the file (§4.3).

### 4.1 The state chunk is the hard constraint

A plugin's state blob is opaque and valid only for the binary that wrote it.
Getting this wrong is worse than not matching at all — a silently mis-restored
chunk is a plugin that loads at wrong settings, or crashes in the vendor's
deserializer. The rule, carried as `twPluginMatch::restoreState`:

- **T0** — restore. Same format, same product.
- **T1/T2 within one format** (a uid the vendor changed between versions) —
  restore.
- **T1/T2 across formats** — restore **only** if the candidate reports the same
  state-compatibility token as the saved chunk. Add an optional
  `stateCompatId()` to the plugin ABI; our SDK can return one token from all
  three wrappers because one serializer sits behind them. Absent the token,
  instantiate at defaults.
- **T3/T4** — never restore. Defaults, always.

Whenever a chunk is dropped, say so in the UI (§5). "Your reverb came back at
default settings" is recoverable; "your reverb came back at *someone else's*
settings" is not.

### 4.2 Policy

One setting, `SOpt::PluginMatchPolicy`:

| value | auto-applies | suggests |
|---|---|---|
| `ExactOnly` | T0 | — |
| `Declared` | T0–T1 | T2–T4 |
| `Learned` *(default)* | T0–T2 | T3–T4 |
| `SameProduct` | T0–T3 | T4 |

Rationale for the default: T0–T2 are either certain or something a human already
confirmed once; T3–T4 are guesses and a guess belongs in a dialog.

### 4.3 What the project file gains

`<SPluginSlot>` grows attributes, all optional, all additive — an older reader
ignores them and a newer reader tolerates their absence:

```
productId='com.Nassau.Mangrove'   the T1 string, when the plugin declared one
version='1.2.0'                   for T2's observed pairing
stateCompat='nassau-sdk-1'        the §4.1 token the chunk was written under
bundle='MangrovePlugin'           path basename, for T4, so a Windows path is
                                  still useful once its drive letter is not
```

And, when a slot was resolved below T0, a **hint** that never replaces identity:

```
resolvedBy='same-product' resolvedFormat='clap' resolvedUid='com.Nassau.Mangrove'
```

`format=`/`uid=` keep saying what the project originally referenced, so carrying
the file back to Windows resolves at T0 there. The hint makes the next load on
*this* machine a T2 hit instead of a T3 guess, and makes the substitution
visible to anyone reading the file.

## 5. Surfacing it

- **A "Plugins in this project" report on load**, shown only when something was
  not T0: one row per slot — what the project asked for, what it got, the tier,
  and whether the state chunk survived. Buttons: Apply (a suggestion),
  Apply all of this tier, Keep missing, Locate… (file picker, which becomes a T2
  user-confirmed alias). Suppressible per project with a "don't ask again" that
  records *keep missing* rather than silence.
- **The effect strip** already greys Missing/Unsupported rows with a reason
  tooltip and a Reload button (proposal 08 M5). Add a badge for "substituted at
  tier X" and "state not restored", with `twPluginMatch::reason` as the tooltip.
- **The log**, one line per non-T0 resolution, in the `plugins` channel.

## 6. Milestones

| M | content | why first |
|---|---|---|
| M1 | B1 + B2 (§3), plus a `plugins`-channel line per unresolved slot | It is the actual defect; the installed Mangrove CLAP starts working, and the report stops lying about what is missing |
| M2 | `twPluginRef`/`twPluginMatch`/`resolve()`/`candidates()`, T0+T3+T4 scoring, the new `<SPluginSlot>` attributes, unit tests against a synthetic registry | The resolver, testable headlessly, with no UI and no persistence |
| M3 | T2: the observed-equivalence record in the scan cache, `pluginaliases.user.json`, and the §5 dialog | The tier that makes the user answer each question once |
| M4 | T1: `productId` through the ABI and all three backends; the shipped `pluginaliases.json`; the Nassau SDK emitting the string | Needs proposal 50's SDK work, and is worthless without M2's plumbing |
| M5 | §4.1 `stateCompatId()` negotiation, so a cross-format match can keep its settings | The one piece that needs vendor cooperation to be more than a default-reset |

M1 is independently shippable and worth doing on its own.

## 7. What this does not fix

The nine slots in §1 whose plugins are on no disk here. For those the deliverable
is the §5 report naming them, and the user installing the macOS builds — for the
four Nassau products, that is proposal 50 M7's output.

## 8. Open questions

- **Q1** — should a T3/T4 substitution, once applied, be *written into* `uid=`
  on the next save, with the original kept in an `originalUid=`? §4.3 says no
  (portability wins). Worth a second opinion if round-tripping through other
  tools ever matters.
- **Q2** — AU on macOS has no cross-platform uid at all, so a Mac-authored
  project is structurally worse off on Windows than the reverse. Should the app
  *prefer* CLAP/VST3 over AU when inserting a plugin that exists in several
  formats, to keep projects portable by construction?

  **Still open, and narrowed (QBX-78, 2026-10-17).** The browser now has a
  **Format** combo that filters the list and is remembered per user
  (`SOpt::PluginBrowserFormat`), so a user *can* say which format they insert
  from, by hand. That is a VIEW filter and deliberately nothing more: it does
  not fold a product's formats into one row and it has no say in what a slot
  resolves to, because both need the cross-format identity of §4's T3/T4,
  which does not exist yet. So Q2 keeps its whole substance — whether the APP
  should prefer a format on the user's behalf, and whether a preference
  expressed while browsing should also steer resolution — and it now has one
  piece of evidence on the "a stored preference must never be rewritten by the
  machine that cannot honour it" side: see `main/pluginui/CONTRACT.md`.
- **Q3** — I/O compatibility for T3/T4: is "candidate is wider" really
  acceptable, or should a width mismatch demote a match one tier?
