# Strategy: Per-Wire Data Format & Sample-Rate Negotiation

## Objective

Give every connection ("wire") between two components an explicit, queryable
**data format** — sample rate, binary sample type, channel count, and layout —
so that the consuming end (the "sink") decides how, or whether, to convert.
Layered on top of that mechanism, define a **project sample rate** that seeds the
whole graph as a sensible default, without making any single rate mandatory at
the wire level.

The proximate motivation is the known sample-rate bug recorded in
`plan/STATE.md`: the synth produces 44.1 kHz, WASAPI shared mode forces the
device mix rate (almost always 48 kHz), and playback is therefore ~8.8 % too
fast and pitched up. But the right fix is not a one-off resampler bolted onto
the speaker — it is to make rate (and format generally) a first-class property
of the signal path, so the speaker is just one sink among many that knows how to
reconcile its input's format with its output's.

## Two perspectives (framing)

**(a) Technical — formats belong to wires, conversion belongs to sinks.**
Today the engine has exactly one implicit format everywhere: mono `float32`,
normalized to `[-1, 1]`, at a hardcoded 44100 Hz
(`tw303a/include/twcomponent.h:15`, `tw303a/src/tw303aenv.cc:7`). Nothing in the
wire carries that assumption — it is baked into every call site. We make the
format explicit and attach it to the wire. A producer advertises what it emits;
a consumer queries it and is free to consume it natively (zero-copy when the
formats already match), convert it, or refuse it with a logged error. No hidden
conversion is forced into the data path.

**(b) Usability — one project rate as the default, never a mandate.**
In practice a user wants a single project sample rate ("this song is a 48 kHz
project") so that everything lines up and there are no surprises. So we add a
project-level rate that seeds `tw303aEnvironment` and becomes the default a
freshly created wire reports. But because the format lives on the wire, a wire
*may* legitimately carry a different rate — an imported 96 kHz one-shot, a
44.1 kHz rendered stem, a future network stream — and the downstream sink
resamples it to whatever it needs. Common rate by default; arbitrary rate where
the signal genuinely demands it.

## Non-goals (for this proposal)

- High-quality / mastering-grade resampling. We specify *where* resampling
  happens and the interface for it; the first implementation may be linear or a
  short polyphase kernel. Swapping in a windowed-sinc resampler later is an
  implementation detail behind the same interface.
- Per-sample dynamic format changes mid-stream. A wire's format is stable for
  the lifetime of a connection; re-negotiation happens on (re)wire, not per
  buffer.
- Control-rate / event (MIDI) signal domains. `twFormat` is designed to be
  extensible toward them (see Open items), but this proposal is about audio
  sample streams only.
- Multi-channel synthesis. The engine core stays mono-per-wire today; the format
  descriptor *can* express N channels so the abstraction is ready, but no
  component is being made multi-channel here.
- Exclusive-mode / device-rate-forcing audio backends. Out of scope; this is the
  graph-side reconciliation, not the device-negotiation side.

## Current state (what we are generalizing)

The wire is the `twLatch` → `twLatchOutput` pair in
`tw303a/include/twcomponent.h`:

```cpp
typedef float sample_t;                 // :15  — the one and only sample type
#define SAMPLE_NORM_MIN (-1.0)
#define SAMPLE_NORM_MAX (1.0)

// :139 — every component fills a float buffer; no rate, no format, no channels:
virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) = 0;

// :105 — the consumer pulls float samples; format is assumed:
length_t readStreamingData( sample_t * pDest, length_t maxLength );
```

Sample rate is reachable only as a single global on the environment, and it is
read-only and hardcoded:

```cpp
// tw303a/include/tw303aenv.h:39
int getSRate() const { return sampleRate; }   // sampleRate = 44100, fixed
```

And the device sink already *knows* there is a mismatch but cannot do anything
about it — it only warns (per STATE.md, `wasapi_backend.cc` logs a
`LOG_WARNING` when the device rate != 44100). The information needed to fix it
(source rate, sink rate) exists; there is just no contract that carries it from
producer to consumer.

## Design

### 1. The format descriptor — `twFormat`

A small value type, attached to a wire, describing the bytes flowing through it.

```cpp
// smaragd/tw303a/include/twformat.h
#ifndef _TWFORMAT_H_
#define _TWFORMAT_H_

#include <cstddef>
#include <cstdint>

// The binary representation of one sample. "not limited to" the engine's
// native float — this is the extension point for any future encoding.
enum class twSampleType : std::uint8_t {
    Float32 = 0,   // sample_t — engine canonical, normalized [-1, 1]
    Float64,
    Int16,         // e.g. WAV writer, ALSA S16_LE, imported PCM
    Int32,
    // … extend here; do NOT renumber existing values (serialized)
};

// How channels are arranged in a multi-channel buffer.
enum class twLayout : std::uint8_t {
    Interleaved = 0,   // L R L R …   (matches the speaker fan-out today)
    Planar,            // L L … R R …
};

struct twFormat {
    std::uint32_t sampleRate = 44100;
    twSampleType  sampleType = twSampleType::Float32;
    std::uint16_t channels   = 1;                      // mono == today's default
    twLayout      layout     = twLayout::Interleaved;

    std::size_t bytesPerSample() const;                // by sampleType
    std::size_t bytesPerFrame()  const { return bytesPerSample() * channels; }

    bool operator==( const twFormat & ) const;
    bool operator!=( const twFormat &o ) const { return !(*this == o); }

    // Can a consumer treat a buffer of `*this` as `other` with a plain memcpy?
    // (same type, channels, layout — rate is metadata, not memory shape)
    bool sameMemoryShape( const twFormat &other ) const;
};

// The engine's canonical exchange format at a given rate: mono float32.
// Every legacy code path is exactly this with rate == env.getSRate().
inline twFormat twCanonicalFormat( std::uint32_t rate ) {
    return twFormat{ rate, twSampleType::Float32, 1, twLayout::Interleaved };
}

#endif
```

The key property: **the default-constructed `twFormat` is byte-for-byte what the
engine produces today** (mono float32). Existing code that never touches formats
keeps working because the default *is* the status quo.

### 2. The wire carries the format

`twFormat` lives on the producing side (the `twLatch`), because only the
producer authoritatively knows what it emits. The consumer reads it through its
`twLatchOutput` view.

```cpp
// twcomponent.h — additions to the existing classes

class twLatch /* : public QObject */ {
    // …
public:
    // Native format of the data this latch produces. Default implementation
    // returns the canonical mono-float32 format at the environment rate, so
    // every existing latch reports the truth without code changes.
    virtual twFormat getFormat() const;
};

class twLatchOutput /* : public QObject */ {
    // …
public:
    // Delegates to the parent latch. The sink's single entry point for "what
    // am I about to read?"
    twFormat getFormat() const { return getParentLatch().getFormat(); }
};
```

A producer that emits something other than canonical (a sample player holding a
44.1 kHz int16 one-shot, a WAV-backed source) overrides `getFormat()` on its
latch. Nothing else has to change for the information to propagate.

Note the distinction `getFormat()` introduces and §3 builds on: it returns the
*currently negotiated* format, the single fixed format flowing right now. That is
distinct from what the wire *could* carry, which is the capability set used to
decide that format (§3).

### 3. Format negotiation — the (re)negotiation protocol

The read-time "sink decides" model (§4) is correct for steady state, but it
cannot answer questions like "the audio driver just switched from 48 kHz to
44.1 kHz — what should the graph do now?" That decision is not sink-local: the
right response might be upstream (the synth core produces 44.1 kHz directly and
the speaker stops resampling), and intermediate nodes have their own constraints
(a mixer needs all inputs at one rate; a resampler bridges only certain ratios;
a sample player is locked to its file's rate). It is a **constraint-satisfaction
over the connected subgraph**, it can be triggered by *any* node, and — because
resolving it may require expensive per-node setup — it must run **off the
realtime path, before playback**, not inside a read call.

So negotiation is a distinct, non-realtime phase with three pieces: capability
advertisement, a negotiation pass, and signalling.

#### 3a. Capabilities — and why they form a *relation*, not two functions

The naïve interface — "ask each node for its output caps and its input caps" —
hides a circularity: a node's output caps usually *depend on* its input caps and
vice versa. A passthrough filter (`twmoog`) emits exactly what it ingests
(`outRate == inRate`); a mixer's output rate is whatever common rate its inputs
agree on; only a resampler decouples the two. So in/out caps are not two
independent snapshots — they are a **coupling relation** the node imposes between
its ports. Resolving the graph is therefore a **fixpoint**, not a single
forward or backward pass: the constraint flows both ways and you iterate until
nothing changes.

Two consequences must be designed in, or the fixpoint may never settle:

1. **Finite, discrete domain.** Caps over an unbounded continuous rate space
   (any rate in ℕ) can ping-pong or crawl without converging. So the negotiator
   works over a finite **candidate set**:

   ```
   D = { standard rates: 44100, 48000, 88200, 96000 }      // the "magnets"
       ∪ { concrete rates hard-anchored by a real source/device in THIS graph }
   ```

   The standard-rate set defaults to **{44100, 48000, 88200, 96000}** and is
   **configurable** — it lives on `tw303aEnvironment` (a project setting), so a
   user can add e.g. 176400/192000 or trim the set, and the negotiator picks it
   up. Nodes only ever *narrow within* `D` — they never invent a fresh rate
   mid-iteration. This is the concrete form of "tame it with required rates like
   44.1 and 48": those standard rates make the domain finite (the real
   termination guarantee), seed the convergence, and double as the default.

2. **Monotone narrowing.** Each node's relation may only ever *remove* candidates
   from a port's domain, never add. A strictly-decreasing sequence over a finite
   lattice reaches a fixpoint in at most `Σ|port domains|` steps — textbook
   arc-consistency (AC-3). The iteration count is thus *bounded by construction*
   (≈ ports × ~6 rates — tiny); an explicit iteration cap is only a **backstop**
   that, if ever hit, means a node violated monotonicity, i.e. a bug to assert
   on — not a routine "good enough, stop here".

```cpp
// twformat.h
struct twFormatCaps {                          // a port's candidate domain ⊆ D
    std::vector<std::uint32_t> rates;          // empty = any (∩ D at pass start)
    std::vector<twSampleType>  types;          // empty = any
    std::vector<std::uint16_t> channelCounts;  // empty = any
    std::uint32_t preferredRate = 0;           // 0 = defer to project rate
};

// All ports of one node, shared so a relation can narrow both sides at once.
struct twPortDomains {
    std::vector<twFormatCaps> in;   // index = input plug
    std::vector<twFormatCaps> out;  // index = output latch
};

// twcomponent.h — additions
class twComponent {
public:
    // SEED domains only — the initial possibilities for each port, before any
    // coupling is applied. Default: canonical mono float32 at any rate in D.
    virtual twFormatCaps getOutputCaps( idx_t idx ) const;
    virtual twFormatCaps getInputCaps ( idx_t idx ) const;

    // THE COUPLING RELATION, iterated to fixpoint by the negotiator. Apply this
    // node's in↔out constraint to the current tentative domains, narrowing them
    // to mutual consistency. MUST be monotone (remove-only) — that is what makes
    // the fixpoint terminate. Returns true iff it narrowed anything.
    //   passthrough/filter: intersect in[i].rates with out[i].rates both ways
    //   mixer:              force all in[].rates to one common rate == out[0]
    //   resampler:          no rate coupling (the decoupling node) — returns false
    //   source/sink:        clamp the open side to the anchored rate
    virtual bool narrowCaps( twPortDomains &ports ) const;

    // COMMIT step, called once after the fixpoint fixes a single format per port.
    // The node records its chosen formats and does its heavy, node-specific setup
    // (build the resampler kernel for the now-known ratio, allocate buffers, init
    // an SRC library). This is where the "proprietary computing" actually runs —
    // off the realtime path. Returns false if the committed format is unworkable.
    virtual bool commitFormats( const twFormat *in,  idx_t nIn,
                                const twFormat *out, idx_t nOut );
};
```

Defaults preserve today's behavior exactly: seed caps = canonical at any rate;
`narrowCaps` enforces `outRate == inRate` for the single-in/single-out case
(passthrough — what every current component effectively is); `commitFormats`
adopts the chosen format with no setup. A component overrides only where it has
real structure (mixer, resampler, sample player, speaker).

#### 3b. The negotiation pass — a terminating fixpoint

A graph-level driver (`twNegotiator`, owned by `tw303aEnvironment` or the
application) resolves the connected subgraph feeding a transport target:

1. **Build the domain.** `D` = standard rates ∪ rates hard-anchored in this
   graph (device rate from `backend_->getConfig()`, fixed-rate sample players).
   Seed every port's domain from `getInputCaps`/`getOutputCaps`, intersected
   with `D`. The project rate (§5) is a *soft* preference for the final pick, not
   a member-deciding anchor.
2. **Propagate to fixpoint (AC-3).** Work a queue of nodes; for each, call
   `narrowCaps`. Whenever a port domain shrinks, re-enqueue the node on the other
   end of its wires. Because narrowing is monotone over the finite lattice `D`,
   the queue drains in bounded steps. (`twRewire`'s per-output latches are
   treated as independent wires.)
3. **Heal empty wires with resamplers.** If a wire's two sides narrow to disjoint
   rate sets (in ∩ out = ∅), insert a `twResampler` on it — which replaces the
   `out==in` relation with "free" — and re-propagate. Each wire is healed at most
   once, so this outer loop also terminates.
4. **Resolve residual freedom (the taming/tiebreak).** After the fixpoint a port
   may still hold several candidates. Collapse each to one by preference order:
   project rate → device rate → standard-rate ranking. This is where 44.1/48 act
   as magnets that pick a single sensible value.
5. **Commit.** Call `commitFormats` on every node (heavy per-node setup runs
   here). Each plug's `getFormat()` now returns its single negotiated format;
   buffers and resampler state are allocated. Only after a successful commit may
   transport start.
6. **Fail loud.** If a wire cannot be healed (e.g. converter insertion is
   disallowed) or a `commitFormats` returns false, the pass reports the offending
   nodes and refuses to start — never silently mis-plays.

This is the up-front analogue of GStreamer caps negotiation: capabilities are
discrete candidate sets, a monotone fixpoint fixes one format per wire,
converters are the free variable that bridges incompatible neighbors, and the
standard rates guarantee the whole thing settles.

#### 3c. Signalling

Renegotiation is event-driven, debounced, and only ever runs while stopped (or
forces a stop):

```cpp
class twComponent {
signals:
    // A node's constraints changed (driver reopened at a new rate, sample
    // loaded, project rate changed). The negotiator schedules a pass before the
    // next play. origin lets the negotiator scope the affected subgraph.
    void renegotiationRequired( twComponent *origin );

    // Emitted during commit when a plug's negotiated format changed, so the node
    // and its UI can react (re-label, re-allocate). Carries old/new.
    void formatChanged( idx_t idx, twFormat oldFmt, twFormat newFmt );
};
```

- **Driver-change scenario (your example).** The backend detects the device
  moved to 44.1 kHz (or is reopened there) → speaker emits
  `renegotiationRequired`. While stopped, the negotiator reruns: if the synth
  core can now produce 44.1 kHz directly, the speaker's resampler collapses to a
  passthrough; resampler state is rebuilt where still needed. The hot path
  restarts with fixed formats.
- **Mid-play change.** The realtime callback never renegotiates. If a change
  arrives during playback, the negotiator stops transport, renegotiates, and
  restarts — a brief, audible-once glitch on device change, which is acceptable
  and honest. (A future refinement could let the speaker absorb a pure
  output-rate change locally without a full pass; deferred — see Open items.)

#### 3d. Relationship to §4 (read-time conversion)

Negotiation decides the format on every wire *before* play; the §4 read path is
what runs *after*, on the hot path, once formats are fixed. Post-negotiation the
sink almost always reads a format it already accepts (the negotiator arranged
that), so the steady-state read is the zero-conversion fast path. §4's
"sink decides / convert / refuse" logic remains the contract for any residual
runtime mismatch and for sinks (like the device speaker) that own a converter by
design rather than via an inserted graph node.

### 4. Steady-state read & conversion (post-negotiation)

After a successful negotiation pass, the consumer reads the format it was
promised. The contract is still: **the consumer inspects `getFormat()` and acts.**
Three honest outcomes, no hidden behavior:

1. **Match → consume natively.** `getFormat() == myWanted` ⇒ read straight
   through, zero conversion. This is the universal case today.
2. **Mismatch, convertible → convert explicitly.** The sink runs the data
   through a converter it owns (or that the negotiator inserted as a graph
   node — see §6).
3. **Mismatch, not handled → refuse + log.** Better a clear
   `LOG_ERR("unsupported wire format …")` than silent wrong-pitch playback.

To support (1) and (2) without breaking (existing float consumers), the read API
gains a native-bytes path alongside the existing canonical-float path:

```cpp
class twLatchStreamingOutput : public twLatchOutput {
public:
    // EXISTING — unchanged contract: canonical mono float32 at env rate.
    // Now formally defined as "native data converted to canonical", which for
    // the common case (native == canonical) is a straight passthrough.
    length_t readStreamingData( sample_t *pDest, length_t maxLength );

    // NEW — native bytes, zero conversion. `dest` must hold
    // maxFrames * getFormat().bytesPerFrame() bytes. Returns frames read.
    // The sink owns the decision of what to do with non-canonical data.
    length_t readRaw( void *dest, length_t maxFrames );
};
```

Conversion itself is a free, shared utility — never duplicated per sink:

```cpp
// smaragd/tw303a/include/twconvert.h
// Pure format conversion (type + channel + layout), NO sample-rate change.
length_t twConvertFrames( const twFormat &src, const void *srcBuf,
                          const twFormat &dst, void       *dstBuf,
                          length_t frames );

// Sample-rate conversion. Stateful (holds filter/interpolator state across
// calls), so it is an object, not a free function. Operates on canonical
// float32 mono; chain it with twConvertFrames at the boundaries.
class twResampler {
public:
    twResampler( std::uint32_t inRate, std::uint32_t outRate );
    // Pull model: asks `src` for as many input frames as it needs to produce
    // `outFrames` output frames. Returns frames actually produced.
    length_t process( twLatchStreamingOutput *src,
                      sample_t *outBuf, length_t outFrames );
    void reset();
};
```

`readStreamingData` keeping its exact old contract is what makes this
non-breaking: every component that pulls floats today continues to, and only
sinks that *care* about native format reach for `readRaw` / `twConvert`.

### 5. The project sample rate (usability layer)

Make the environment rate settable and persisted, and let it seed every wire's
default format.

```cpp
// tw303aenv.h
class tw303aEnvironment {
    int sampleRate;   // already exists; default flips to a project setting
    std::vector<std::uint32_t> candidateRates_ =        // NEW — the standard set D
        { 44100, 48000, 88200, 96000 };                 // configurable; persisted
public:
    int  getSRate() const { return sampleRate; }
    void setSRate( int rate );   // NEW — emits sampleRateChanged

    // The standard-rate magnets the negotiator builds D from (§3a). User-
    // editable (add 176400/192000, trim, …); persisted with the project.
    const std::vector<std::uint32_t> &candidateRates() const { return candidateRates_; }
    void setCandidateRates( std::vector<std::uint32_t> rates );  // emits candidateRatesChanged
signals:
    void sampleRateChanged( int oldRate, int newRate );   // NEW
    void candidateRatesChanged();                         // NEW — triggers renegotiation
};
```

Changing the project rate shifts every default wire's preferred format, so
`setSRate` is one of the triggers that schedules a negotiation pass (§3c): the
environment translates `sampleRateChanged` into `renegotiationRequired` on the
affected components. The new rate takes effect on the next (re)negotiation while
transport is stopped, not mid-buffer.

- **Project state.** The project sample rate is stored in the project XML
  (one attribute on the root, defaulting to 44100 on load of older files so they
  round-trip unchanged). A `File → New` project picks a default — recommend
  **48000** to match the overwhelming majority of modern default-output devices
  and eliminate the resample on the hot path for most users. (Tunable; the point
  is that there *is* a project rate, not which number.)
- **Seeds, does not mandate.** `twCanonicalFormat(env.getSRate())` is what a
  default latch reports. A wire that genuinely carries another rate overrides
  its `getFormat()`; the project rate is the default, not a lock.
- **Rate-aware components.** The literal `44100`s scattered through the engine
  (oscillator pitch `4410000/currFreq` in `twsaw.cc`/`twsimplesaw.cc`, the
  1-second delay line in `twpipe.cc`, the Nyquist term in `twmoog.cc`, the tempo
  math in `twtestseq.cc`, the WAV header in `twwav.cc`) must consult
  `env.getSRate()` instead of the literal. This is the bulk of the "make the
  synth rate-aware" work and is what lets the project actually *run* at the
  chosen rate rather than just resampling a 44.1 kHz core at the end.

### 6. Where conversion physically happens

Two distinct boundaries, two strategies:

**Inside the graph — explicit converter nodes inserted by negotiation.** When
component A's output caps cannot meet consumer B's input caps, the negotiation
pass (§3b step 2) inserts a `twResampler` / format-convert node on that wire and
fixes the formats on either side of it. Because the converter is a real graph
node, conversion is visible and inspectable, never smuggled into a read call —
and the node builds its (possibly expensive) state during the off-realtime
negotiation phase, not on the hot path.

**At the device boundary — the speaker owns its resampler.** The device mix
rate is negotiated at runtime by the backend (`AudioConfig.sampleRate` in
`tw303a/include/audio/audio_backend.h:13`) and is *not* part of the project
graph. So `twSpeaker` holds a `twResampler` internally: it reads its input wire
at the input's rate and resamples to `backend_->getConfig().sampleRate` inside
the render callback. This is the concrete fix for the 8.8 % pitch bug. The
speaker's current fan-out callback (mono float in, interleaved N-channel float
out) gains one stage:

```
pInputPlugs[0] (rate = wire format)
        │ readStreamingData → mono float32 @ wire rate
        ▼
   twResampler(wireRate → deviceRate)        // no-op when rates already equal
        │ mono float32 @ device rate
        ▼
   mono → N-channel interleave (unchanged)
        ▼
   backend render buffer (float, device rate)
```

When `wireRate == deviceRate` the resampler is a passthrough, so the common
matched case pays nothing.

### 7. Backend format reporting

`AudioConfig` already carries `sampleRate`, `channels`, `bufferFrames`,
`periodFrames` (`audio_backend.h:12`). It gains nothing structurally — it simply
becomes the authority the speaker's resampler targets. Optionally expose the
device's native `twSampleType` so the speaker can hand the backend its preferred
binary format directly (ALSA wants S16, WASAPI may want float32/int16/int32 per
STATE.md) via `twConvertFrames`, instead of each backend re-implementing the
float→int conversion it does today.

## Worked examples

**Speaker reconciling 44.1 kHz synth with a 48 kHz device** (the bug):

```cpp
// inside twSpeaker's render callback (sketch)
twFormat in = pInputPlugs[0]->getFormat();          // 44100, Float32, mono
auto      dev = backend_->getConfig();              // 48000, 2ch
if( !resampler_ || resampler_.inRate() != in.sampleRate
                || resampler_.outRate() != dev.sampleRate )
    resampler_.reset( in.sampleRate, dev.sampleRate );

length_t got = resampler_.process(                  // mono @ 48000
    static_cast<twLatchStreamingOutput*>(pInputPlugs[0]), scratch_, frames );
// then existing mono → N-channel interleave into `out`
```

**WAV writer consuming int16 natively** (no float roundtrip): `twWav`'s sink
queries `getFormat()`; if the upstream latch already reports `Int16` at the file
rate it `readRaw`s straight into the WAV data chunk; otherwise it runs
`twConvertFrames(in, …, {fileRate, Int16, …}, …)`. Either way the conversion
decision is the sink's and is explicit.

**Imported 96 kHz one-shot in a 48 kHz project**: the sample-player latch
overrides `getFormat()` to report `96000`. The project rate is 48000, so the
consumer (or an auto-inserted graph node) resamples 96000 → 48000. Nothing
forces the sample to be pre-converted on import; the wire tells the truth and
the sink reconciles.

## Migration / backward compatibility

- Default `twFormat` == mono float32 == today's universal assumption ⇒ existing
  components compile and behave identically with no edits.
- `readStreamingData` keeps its exact contract (canonical float32); only new
  sinks use `readRaw`.
- Old project files have no rate attribute ⇒ load as 44100, re-save with the
  attribute present. (Mirrors the v1→v2 file-format approach in
  `03_ACTION_MODEL.md`.)
- The 8.8 % bug is fixed the moment the speaker resampler lands (§6), even
  before any oscillator is made rate-aware and before full graph negotiation
  exists — because the synth keeps producing at its native 44100 and the speaker
  reconciles locally. Making the core rate-aware (§5) and adding the negotiation
  pass (§3) are the follow-ups that let the project genuinely *be* 48 kHz and let
  a driver-rate change ripple through the whole graph.

## Phased rollout

1. **Descriptor + wire metadata.** Land `twformat.h`, `twLatch::getFormat()`
   (default = canonical), `twLatchOutput::getFormat()`. No behavior change; pure
   addition. Verify the default reports mono/float32/env-rate everywhere.
2. **Speaker resampler.** Add `twResampler` (linear first) and wire it into
   `twSpeaker`'s render callback against `backend_->getConfig().sampleRate`.
   **This closes the pitch bug.** Acceptance: a 44.1 kHz project plays at correct
   pitch/speed on a 48 kHz default device.
3. **Native read path + converters.** Add `readRaw`, `twConvertFrames`. Port the
   per-backend float→int conversion in `alsa_backend.cc` / `wasapi_backend.cc`
   onto the shared converter. Port `twWav` to query format and write int16
   natively.
4. **Project sample rate.** `setSRate` + `sampleRateChanged`, project-XML
   attribute, `File → New` default (recommend 48000), load/save round-trip of
   older files.
5. **Rate-aware engine.** Replace the literal `44100`s
   (`twsaw`, `twsimplesaw`, `twpipe`, `twmoog`, `twtestseq`, `twwav`) with
   `env.getSRate()`. After this the resampler in step 2 becomes a passthrough
   for matched-rate projects.
6. **Capabilities.** Add `getInputCaps`/`getOutputCaps` (seed = canonical at any
   rate) and the `narrowCaps` coupling relation (default = passthrough
   `outRate==inRate`); override both on the nodes with real structure (mixer,
   resampler, sample player, device speaker). Pure addition; no pass yet.
7. **Negotiation pass + signalling.** Add `twNegotiator` (build domain `D`,
   AC-3 fixpoint, resampler healing, preference resolve, `commitFormats`) and
   `renegotiationRequired`/`formatChanged`. Run a pass before `startOutput()`;
   wire `setSRate` and the backend's device-rate change into
   `renegotiationRequired`. This generalizes the step-2 speaker-local resampler
   into whole-graph reconciliation and delivers the driver-switch scenario.

Steps 1–2 are the minimum to fix the reported bug. 3–5 deliver the per-wire
format mechanism and a genuinely rate-aware core; 6–7 deliver the negotiation
protocol that lets any node trigger reconciliation (the driver-change case).

## Open items (intentionally deferred)

- **Resampler quality.** Start linear; the interface admits a polyphase /
  windowed-sinc upgrade with no call-site changes.
- **Signal domains beyond audio.** `twFormat` is structured to grow a `domain`
  field (audio / control-rate / event) so the same wire-metadata machinery can
  later describe MIDI or modulation wires. Not specified here.
- **Multi-channel synthesis.** `channels`/`layout` exist in the descriptor but no
  component produces > 1 channel yet; true stereo/surround synthesis is a
  separate effort.
- **Automatic vs. manual converter insertion** (§3b/step 7) — whether the
  negotiator silently inserts adapters or requires explicit user/graph action is
  a UX decision to settle when that step is reached.
- **Negotiation authority / anchor priority.** When constraints conflict — e.g. a
  fixed-rate source vs. a fixed-rate device — which wins, and does a source ever
  get to force the *device* to reopen at its rate (exclusive-mode territory)?
  Proposal's default: device rate is the hard anchor, sources resample to it;
  revisit if a "bit-perfect" mode is wanted.
- **Mid-play local absorption.** §3c stops/renegotiates/restarts on a mid-play
  device change (one glitch). A refinement could let the speaker absorb a pure
  output-rate change by just rebuilding its own resampler ratio without a full
  graph pass. Deferred until the glitch proves annoying.
- **Negotiation failure UX.** What the user sees when a pass fails (infeasible
  caps, converter not permitted) — a dialog, a disabled Play button, a
  highlighted offending node. Surface, never silently mis-play.
- **The standard-rate table.** *Decided:* `D`'s standard set defaults to
  **{44100, 48000, 88200, 96000}** and is **configurable** via
  `tw303aEnvironment::setCandidateRates` (persisted with the project), so a user
  can add 176400/192000 or trim the set. Still to confirm during implementation:
  the monotone-narrowing invariant should carry a debug assertion so the
  iteration backstop never trips in normal operation, and whether to auto-extend
  `D` with rates a connected device advertises (vs. only hard-anchored ones).
- **Interaction with `03_ACTION_MODEL.md`.** Changing the project sample rate is
  itself a project mutation and should eventually be an `SAction`
  (`set-project-rate`) so it is undoable and scriptable. Cross-referenced; not
  blocking.

## Acceptance criteria

- A project authored at 44.1 kHz plays at correct pitch and speed through a
  48 kHz default Windows device (the reported bug is gone), with the speaker
  resampler the only thing in the path.
- A matched-rate project (project rate == device rate) incurs no resampling —
  verified by the resampler reporting passthrough.
- An imported sample at a foreign rate plays at correct pitch in a project of a
  different rate.
- The WAV writer produces a correct int16 file without a float→int→float
  roundtrip, sourcing format from the wire.
- Existing project files load unchanged, gain a rate attribute on save, and
  reload identically.
- No existing component required edits to keep working after step 1 (default
  format == prior behavior).
- **Driver-change renegotiation:** with the device switched from 48 kHz to
  44.1 kHz while stopped, hitting Play renegotiates the graph and plays at
  correct pitch — and if the core is rate-aware, with the speaker resampler
  collapsed to a passthrough rather than resampling 44.1→44.1.
- A negotiation pass that cannot satisfy the graph's constraints refuses to
  start transport and surfaces the offending node, rather than playing wrong.
