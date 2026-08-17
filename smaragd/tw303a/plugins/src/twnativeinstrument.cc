// twNativeInstrument — the in-repo 303 voice, as a twPlugin (proposal 37 D7, P2).
//
// WHY THIS EXISTS. The instrument path (event delivery, chase, pre-roll, the run
// barrier, the byte-`cmp` determinism gates) has to be built and gated against
// an instrument that needs no SDK, no submodule and no third-party binary, in
// every build and every worktree. `tw.test.clap.sine` and the VST3 `TestSine`
// cover "a real plugin format carries notes"; THIS one covers "the engine has an
// instrument at all", and it is the one a user can reach from the browser
// exactly like the built-in `tw.passthrough` (twPluginRegistry::appendBuiltins).
//
// WHAT IT IS. A monophonic TB-303-shaped voice:
//
//   saw or square oscillator -> 4-pole ladder low-pass -> amplitude
//        ^ portamento (slide)      ^ cutoff + envMod * decay envelope
//
// The oscillator and the ladder arithmetic are LIFTED from the graph components
// (tw/dsp twSimpleSaw and twMoog) into buffer-level functions, per design §5.6:
// those components are audio-rate-control-input graph nodes with no note or
// voice model, the app never instantiates any of them, and proposal 12's "first
// consumer" never materialised. They stay where they are for the tests that use
// them; nothing here calls them.
//
// The ladder is twMoog's structure verbatim (the 0.3 feed-forward, the (1-f)
// pole, the resonance feedback with its 1 - 0.15 f^2 term) with two deliberate
// changes: it runs in the FLOAT domain [-1,1] rather than twMoog's int16-scaled
// one, so the ±32767 pole clamps become ±4.0 guards that only ever fire on a
// self-oscillating runaway; and `f` is computed per sample from the envelope
// instead of read from an audio-rate input latch.
//
// DETERMINISM IS A CONTRACT, not an accident (dsp invariant 2, proposal 37 AC5).
// reset() returns every piece of state to a fixed value — phase 0, all eight
// ladder registers 0, envelope 0, no note held, glide target = current — so
// "reset, note-on at offset 0, render N frames" is byte-identical every time and
// in every process. There is no dither, no random seed, no time source, and no
// denormal-dependent branch.
//
// MONOPHONIC, WITH SLIDE. A NoteOn while another note is held does NOT restart
// the envelope: it retunes the running voice with portamento, which is what a
// 303's slide is. The held-note stack is LIFO, so releasing the newer note
// slides back to the older one — the behaviour a step sequencer's overlapping
// gates produce.
//
// ACCENT is velocity >= 100/127 (design §5.3: "velocity >= 100 = accent"), which
// is the normalised spelling of MIDI velocity 100 — the ABI carries velocity in
// [0,1] (see twpluginevents.h). An accented note gets more envelope into the
// cutoff and more level, the two things a 303's accent circuit does.

#include "tw/plugins/twplugin.h"

#include "tw/core/twlog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace audio {

namespace {

constexpr std::uint32_t kMaxHeld = 16;   // held-note stack depth

// Parameter ids. Stable: they are what an automation lane and a saved project
// will quote, so they may be appended to but never renumbered.
enum : std::uint32_t {
    kParamCutoff    = 0,   // Hz, 20 .. 12000
    kParamResonance = 1,   // 0 .. 1
    kParamEnvMod    = 2,   // 0 .. 1, how much envelope reaches the cutoff
    kParamDecay     = 3,   // seconds, 0.02 .. 4
    kParamAccent    = 4,   // 0 .. 1, how much an accented note adds
    kParamWaveform  = 5,   // stepped: 0 = saw, 1 = square
    kParamSlide     = 6    // seconds of portamento, 0 .. 0.5
};
constexpr std::uint32_t kNumParams = 7;

// Velocity at or above which a note is ACCENTED. MIDI velocity 100 normalised.
constexpr double kAccentVelocity = 100.0 / 127.0;

// The VCA release, in seconds. SHORT and fixed, and deliberately NOT the
// `decay` parameter: on a real 303 the Decay knob sweeps the FILTER envelope
// while the VCA is a near-rectangular gate, so a released note stops promptly
// however long the filter sweep is set. Two consequences that matter here:
// a note-off is audible immediately (with a ramp, so no click), and the audible
// tail is 6 ms rather than up to four seconds — which is what lets a test assert
// "the note ended" sharply instead of watching a slow slope.
constexpr double kReleaseSec = 0.006;

constexpr std::size_t kStateHeaderSize = 8;
constexpr std::uint8_t  kStateMagic[4] = { 'T', 'W', 'N', 'I' };
constexpr std::uint16_t kStateVersion  = 1;

double midiKeyToHz( int key )
{
    return 440.0 * std::pow( 2.0, ( (double)key - 69.0 ) / 12.0 );
}

void putU16le( std::uint8_t *p, std::uint16_t v )
{
    p[0] = (std::uint8_t)( v & 0xFF );
    p[1] = (std::uint8_t)( ( v >> 8 ) & 0xFF );
}

std::uint16_t getU16le( const std::uint8_t *p )
{
    return (std::uint16_t)( (std::uint16_t)p[0] | ( (std::uint16_t)p[1] << 8 ) );
}

}  // namespace

class twNativeInstrument final : public twPlugin {
public:
    twNativeInstrument()
    {
        io_.audioInputs  = 0;   // a generator: no audio in
        io_.audioOutputs = 1;   // mono, like every other page in this engine

        params_ = {
            { kParamCutoff,    "Cutoff",    20.0, 12000.0, 800.0, false },
            { kParamResonance, "Resonance",  0.0,     1.0,   0.7, false },
            { kParamEnvMod,    "Env Mod",    0.0,     1.0,   0.6, false },
            { kParamDecay,     "Decay",     0.02,     4.0,   1.5, false },
            { kParamAccent,    "Accent",     0.0,     1.0,   0.5, false },
            { kParamWaveform,  "Waveform",   0.0,     1.0,   0.0, true },
            { kParamSlide,     "Slide",      0.0,     0.5,  0.06, false },
        };
        for( const twPluginParamInfo &p : params_ )
            v_[p.id] = p.defaultValue;

        reset();
    }

    const twPluginIoLayout &ioLayout() const override { return io_; }

    void prepare( std::uint32_t sampleRate, std::uint32_t maxBlock ) override
    {
        (void)maxBlock;   // nothing here is sized by the block length
        if( sampleRate == 0 )
            sampleRate = 48000;
        if( sampleRate != rate_ ) {
            rate_ = sampleRate;
            reset();   // every coefficient is rate-derived; a stale one whistles
        }
    }

    void reset() override
    {
        phase_       = 0.0;
        held_.clear();
        held_.reserve( kMaxHeld );
        gate_        = false;
        env_         = 0.0;
        amp_         = 0.0;
        vca_         = 0.0;
        accent_      = false;
        freqHz_      = 0.0;
        targetHz_    = 0.0;
        glideRemain_ = 0.0;
        o1_ = o2_ = o3_ = o4_ = i1_ = i2_ = i3_ = i4_ = 0.0;
    }

    // --- parameters ---------------------------------------------------------

    std::size_t       paramCount() const override { return params_.size(); }
    twPluginParamInfo paramInfo( std::size_t i ) const override
    {
        return i < params_.size() ? params_[i] : twPluginParamInfo{};
    }
    double getParam( std::uint32_t id ) const override
    {
        return id < kNumParams ? v_[id] : 0.0;
    }
    void setParam( std::uint32_t id, double v ) override
    {
        if( id >= kNumParams )
            return;   // an unknown id is refused, never invented
        const twPluginParamInfo &p = params_[id];
        v_[id] = std::min( p.maxValue, std::max( p.minValue, v ) );
    }

    std::string paramValueText( std::uint32_t id, double v ) const override
    {
        char buf[64];
        switch( id ) {
        case kParamCutoff:   std::snprintf( buf, sizeof( buf ), "%.0f Hz", v ); break;
        case kParamDecay:    std::snprintf( buf, sizeof( buf ), "%.2f s", v ); break;
        case kParamSlide:    std::snprintf( buf, sizeof( buf ), "%.0f ms", v * 1000.0 ); break;
        case kParamWaveform: return v >= 0.5 ? "Square" : "Saw";
        default:             std::snprintf( buf, sizeof( buf ), "%.0f %%", v * 100.0 ); break;
        }
        return std::string( buf );
    }

    // --- capabilities -------------------------------------------------------

    twPluginCapabilities capabilities() const override
    {
        twPluginCapabilities c;
        c.acceptsNotes   = true;
        c.emitsNotes     = false;
        c.isInstrument   = true;
        c.supportsNoteIds = true;    // matched by id when the host issues one
        c.notePortsIn    = 1;
        c.notePortsOut   = 0;
        return c;
    }

    // The AUDIBLE tail is the VCA release (6 ms). The number reported here is
    // the FILTER decay instead, deliberately and conservatively: proposal 37 D4
    // uses tailFrames() to size the instrument pre-roll, i.e. "how far back must
    // a render reach to rebuild the state this instrument carries", and the
    // filter envelope is state that outlives the sound. An over-estimate costs
    // pre-roll frames; an under-estimate costs a wrong note.
    std::uint32_t tailFrames() const override
    {
        const double decay = getParam( kParamDecay );
        return (std::uint32_t)( decay * (double)rate_ );
    }

    // --- state --------------------------------------------------------------

    std::vector<std::uint8_t> saveState() const override
    {
        std::vector<std::uint8_t> blob( kStateHeaderSize, 0 );
        std::memcpy( blob.data(), kStateMagic, sizeof( kStateMagic ) );
        putU16le( blob.data() + 4, kStateVersion );
        putU16le( blob.data() + 6, 0 );
        // Parameters only, in id order, as little-endian doubles. The VOICE is
        // never stored: a patch is what the user set, not which note happened to
        // be sounding at save time.
        for( const twPluginParamInfo &p : params_ ) {
            const double v = getParam( p.id );
            const std::size_t at = blob.size();
            blob.resize( at + sizeof( double ) );
            std::memcpy( blob.data() + at, &v, sizeof( double ) );
        }
        return blob;
    }

    bool loadState( const std::vector<std::uint8_t> &blob ) override
    {
        if( blob.size() < kStateHeaderSize )
            return false;
        if( std::memcmp( blob.data(), kStateMagic, sizeof( kStateMagic ) ) != 0 )
            return false;
        const std::uint16_t ver = getU16le( blob.data() + 4 );
        if( ver > kStateVersion ) {
            TW_LOGW( "plugins", "[tw303] state blob is version %u, we understand %u; "
                     "keeping the defaults", (unsigned)ver, (unsigned)kStateVersion );
            return false;
        }
        // Tolerant of a SHORTER payload (an older build with fewer parameters)
        // and of a longer one (a newer build's extra values are ignored) —
        // CONTRACT invariant 3.
        std::size_t at = kStateHeaderSize;
        for( const twPluginParamInfo &p : params_ ) {
            if( at + sizeof( double ) > blob.size() )
                break;
            double v = 0.0;
            std::memcpy( &v, blob.data() + at, sizeof( double ) );
            at += sizeof( double );
            setParam( p.id, v );
        }
        return true;
    }

    // --- processing ---------------------------------------------------------

    void process( const float *const *in, float *const *out,
                  std::uint32_t nframes ) override
    {
        const twEventList      noEvents{};
        twEventOut             noSink;
        const twProcessContext noCtx{};
        float *const *const    buses[1] = { out };
        process( in, buses, nframes, noEvents, noSink, noCtx );
    }

    void process( const float *const *in, float *const *const *outBuses,
                  std::uint32_t nframes, const twEventList &events,
                  twEventOut &eventsOut, const twProcessContext &ctx ) override
    {
        (void)in;   // a generator ignores its audio input; the HOST sums it
        (void)eventsOut;
        (void)ctx;

        float *o = ( outBuses && outBuses[0] ) ? outBuses[0][0] : nullptr;
        if( !o || nframes == 0 )
            return;

        // Snapshot the parameters once per call: a mid-block parameter change
        // arrives as a ParamValue event and is applied at its own frame below,
        // so reading them per sample from the map would be both slower and
        // no more accurate.
        const double srate = (double)rate_;

        std::uint32_t ev = 0;
        for( std::uint32_t i = 0; i < nframes; ++i ) {
            // Apply every event whose frame has arrived, in list order.
            while( ev < events.count && events.events[ev].time <= (std::int64_t)i ) {
                applyEvent( events.events[ev] );
                ++ev;
            }
            o[i] = renderSample( srate );
        }
        // Anything the host placed past the end of the block still belongs to
        // this call (a clamped event); apply it so it is not silently lost.
        for( ; ev < events.count; ++ev )
            applyEvent( events.events[ev] );
    }

private:
    struct HeldNote {
        std::int16_t key    = -1;
        std::int32_t noteId = -1;
        double       velocity = 0.0;
    };

    void applyEvent( const twEvent &e )
    {
        switch( e.kind ) {
        case twEventKind::NoteOn:
            noteOn( e.key, e.noteId, e.value );
            break;
        case twEventKind::NoteOff:
            noteOff( e.key, e.noteId );
            break;
        case twEventKind::NoteChoke:
            // A choke is immediate and unconditional: no release, no slide back.
            held_.clear();
            gate_  = false;
            env_   = 0.0;
            amp_   = 0.0;
            break;
        case twEventKind::ParamValue:
            setParam( e.paramId, e.value );
            break;
        default:
            // Everything else (CC, bend, expression, metadata) has no mapping
            // here. Silently ignoring is right: a 303 has no mod matrix.
            break;
        }
    }

    void noteOn( std::int16_t key, std::int32_t noteId, double velocity )
    {
        if( key < 0 )
            return;   // a wildcard note-on is meaningless for a monophonic voice
        if( held_.size() >= kMaxHeld )
            held_.erase( held_.begin() );   // oldest loses; the stack is bounded

        const bool wasHeld = !held_.empty();
        HeldNote  n;
        n.key      = key;
        n.noteId   = noteId;
        n.velocity = velocity;
        held_.push_back( n );

        retune( key, wasHeld );
        accent_ = velocity >= kAccentVelocity;
        amp_    = velocity;

        // SLIDE: a note that overlaps a held one retunes the running voice and
        // leaves the envelope alone. Only a note that STARTS the voice retriggers.
        if( !wasHeld )
            env_ = 1.0;
        gate_ = true;
    }

    void noteOff( std::int16_t key, std::int32_t noteId )
    {
        // Match by id when the host issued one (the ABI's rule: a NoteOff
        // carries the same id or -1), else by key, else — a wildcard off —
        // release everything.
        if( key < 0 && noteId < 0 ) {
            held_.clear();
        } else {
            for( std::size_t i = held_.size(); i-- > 0; ) {
                const bool byId  = noteId >= 0 && held_[i].noteId == noteId;
                const bool byKey = noteId < 0 && held_[i].key == key;
                if( byId || byKey ) {
                    held_.erase( held_.begin() + (std::ptrdiff_t)i );
                    break;
                }
            }
        }
        if( held_.empty() ) {
            gate_ = false;
        } else {
            // Slide back to whatever is still down (LIFO).
            retune( held_.back().key, true );
            amp_ = held_.back().velocity;
        }
    }

    void retune( std::int16_t key, bool glide )
    {
        targetHz_ = midiKeyToHz( key );
        if( !glide || freqHz_ <= 0.0 ) {
            freqHz_      = targetHz_;
            glideRemain_ = 0.0;
            return;
        }
        glideRemain_ = getParam( kParamSlide ) * (double)rate_;
        if( glideRemain_ < 1.0 ) {
            freqHz_      = targetHz_;
            glideRemain_ = 0.0;
        }
    }

    float renderSample( double srate )
    {
        // --- portamento: a linear glide over the remaining frames ------------
        if( glideRemain_ > 0.0 ) {
            freqHz_ += ( targetHz_ - freqHz_ ) / glideRemain_;
            glideRemain_ -= 1.0;
            if( glideRemain_ <= 0.0 )
                freqHz_ = targetHz_;
        }

        // --- envelope: one-pole decay while gated, faster release when not ---
        // Both are exp(-1/tau) per sample; the release is deliberately the same
        // curve so tailFrames() bounds it.
        const double decaySec = getParam( kParamDecay );
        const double tau      = std::max( 1.0, decaySec * srate );
        const double coeff    = std::exp( -1.0 / tau );
        env_ *= coeff;
        if( env_ < 1e-9 )
            env_ = 0.0;

        // --- VCA: rectangular gate with a short linear release ---------------
        if( gate_ ) {
            vca_ = 1.0;
        } else if( vca_ > 0.0 ) {
            vca_ -= 1.0 / std::max( 1.0, kReleaseSec * srate );
            if( vca_ < 0.0 )
                vca_ = 0.0;
        }
        const double level = amp_ * vca_;

        // Fully released (or never started): EXACT zero, and the oscillator and
        // filter are left frozen rather than run — so "silence after the
        // note-off" is silence and not a small number, and the state a later
        // note starts from is the same one every run (AC5).
        if( freqHz_ <= 0.0 || vca_ <= 0.0 )
            return 0.0f;

        // --- oscillator ------------------------------------------------------
        phase_ += freqHz_ / srate;
        if( phase_ >= 1.0 )
            phase_ -= std::floor( phase_ );
        const bool   square = getParam( kParamWaveform ) >= 0.5;
        const double osc    = square ? ( phase_ < 0.5 ? 1.0 : -1.0 )
                                     : ( 2.0 * phase_ - 1.0 );

        // --- ladder filter (twMoog's arithmetic, float domain) ---------------
        //
        // Cutoff = base + envMod * envelope, in Hz, with the accent adding both
        // envelope depth and level — the two halves of a 303's accent.
        const double accentAmt = accent_ ? getParam( kParamAccent ) : 0.0;
        const double envAmt    = getParam( kParamEnvMod ) * ( 1.0 + accentAmt );
        double cutoff = getParam( kParamCutoff ) + envAmt * env_ * 8000.0;
        const double nyquist = srate * 0.5;
        cutoff = std::min( cutoff, nyquist * 0.98 );
        cutoff = std::max( cutoff, 20.0 );

        // twMoog: f = cutoff * 1.16 / (srate/2), clamped so the poles stay stable.
        double f = cutoff * 1.16 / nyquist;
        f = std::min( f, 1.16 );
        const double res = getParam( kParamResonance );
        const double fb  = res * 4.0 * ( 1.0 - 0.15 * f * f );

        double input = osc * level * ( 1.0 + accentAmt * 0.5 );
        input -= o4_ * fb;
        input *= 0.35013 * ( f * f ) * ( f * f );

        o1_ = input + 0.3 * i1_ + ( 1.0 - f ) * o1_;   // pole 1
        i1_ = input;
        o2_ = o1_ + 0.3 * i2_ + ( 1.0 - f ) * o2_;     // pole 2
        i2_ = clampGuard( o1_ );
        o3_ = o2_ + 0.3 * i3_ + ( 1.0 - f ) * o3_;     // pole 3
        i3_ = clampGuard( o2_ );
        o4_ = o3_ + 0.3 * i4_ + ( 1.0 - f ) * o4_;     // pole 4
        i4_ = clampGuard( o3_ );
        o4_ = clampGuard( o4_ );

        return (float)o4_;
    }

    // twMoog guards each pole against a runaway; in the int domain the limit was
    // ±32767, here it is ±4.0. It never fires on musical material — it exists so
    // a self-oscillating resonance cannot reach infinity and poison the page.
    static double clampGuard( double v )
    {
        if( v > 4.0 ) return 4.0;
        if( v < -4.0 ) return -4.0;
        return v;
    }

    twPluginIoLayout               io_{};
    // params_ is indexed BY ID: the ids are 0..kNumParams-1 by construction, so
    // paramInfo(i) and v_[id] agree without a lookup.
    std::vector<twPluginParamInfo> params_;
    double                         v_[kNumParams] = { 0 };

    std::uint32_t rate_ = 48000;

    // Voice state. Every field is set by reset(); nothing here is uninitialised
    // or time-dependent (AC5 compares two runs byte for byte).
    std::vector<HeldNote> held_;
    bool   gate_   = false;
    bool   accent_ = false;
    double phase_  = 0.0;
    double env_    = 0.0;   // FILTER envelope (the Decay knob)
    double amp_    = 0.0;   // the held note's velocity
    double vca_    = 0.0;   // amplitude envelope: 1 while gated, releasing after
    double freqHz_ = 0.0, targetHz_ = 0.0, glideRemain_ = 0.0;
    double o1_ = 0, o2_ = 0, o3_ = 0, o4_ = 0, i1_ = 0, i2_ = 0, i3_ = 0, i4_ = 0;
};

// Referenced BY NAME from twPluginRegistry (CONTRACT invariant 1: discovery is
// symbol-referenced, never static-init self-registration).
std::unique_ptr<twPlugin> createNativeInstrument()
{
    return std::unique_ptr<twPlugin>( new twNativeInstrument() );
}

}  // namespace audio
