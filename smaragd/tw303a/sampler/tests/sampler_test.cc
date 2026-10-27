// sampler_test — proposal 56 Q0.1 / Q0.2 at engine level, a direct processor
// test in the style of plugins_test.
//
// What it pins, and why each part can fail:
//
//   A. REGISTRATION BY CALL (B2, invariant 2). A registry that was never told
//      does not know tw.native.nassaudigital; registerNassauDigital() makes it
//      resolvable, listable ONCE (also after a second call and after a rescan),
//      and instantiable — whether the list was already seeded or not.
//   B. THE CAPABILITY. dynamic_cast<twSamplerHost*> answers for the sampler and
//      null for every other built-in.
//   C. THE GENERATOR ROWS of a 0 -> 2 instrument: DirectGen on 2 channels,
//      GenFold on 1, refused (Transparent + Unsupported) on 6.
//   D. INSTALL THROUGH applyToInstances, READ BY process(). Before the install
//      the render is silent; the install moves the insert's content epoch (or a
//      cached page would be served unchanged); after it the note at frame 1000
//      plays the sample, sample for sample, and silence before it.
//      twSamplerHostStats — written BY process() — reports the program serial,
//      the routed mask and the sample count it ran on: the install was read on
//      the audio path, not merely stored.
//   E. A VOICE KEEPS ITS GENERATION (DESIGN §6.1 rule 4): a sample installed
//      between two contiguous renders does not reach the note already sounding.
//   F. INSTALLS RACING RENDERS land between process() calls: one thread renders
//      the same page over and over while another swaps the sample through
//      applyToInstances; every page is exactly the A render or exactly the B
//      render, never a mixture.
//   G. THE STATE BLOB HOLDS NO SAMPLE (A3, the engine half of Q0.4): it is the
//      8-byte header before and after a 10 MB sample is installed, and a newer
//      version is refused.

#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplerregistry.h"
#include "tw/sampler/twsamplertypes.h"

#include "tw/events/tweventsource.h"
#include "tw/graph/tw303aenv.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twpluginslotproc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace audio;

namespace {

int gFailures = 0;

bool check( bool ok, const std::string &what )
{
    if( ok ) std::cout << "  ok   " << what << std::endl;
    else {
        std::cerr << "  FAIL " << what << std::endl;
        ++gFailures;
    }
    return ok;
}

// One held note, in the MODEL's MIDI domain (velocity 0..127), which is what a
// real feed carries and what the processor normalizes on its way to the ABI.
class OneNoteSource : public twEventSource {
public:
    OneNoteSource( std::int64_t start, std::int64_t dur, std::int16_t key, double vel )
        : start_( start ), dur_( dur ), key_( key ), vel_( vel ) {}

    void collect( std::int64_t startPos, std::int64_t len, twEventBlock &out ) const override
    {
        out.clear();
        if( len <= 0 ) return;
        const std::int64_t end = startPos + len, nEnd = start_ + dur_;
        if( start_ < startPos && nEnd > startPos ) {
            twHeldNote h;
            h.key = key_; h.channel = 0; h.velocity = vel_; h.noteId = 0;
            h.start = start_; h.duration = dur_; h.srcIndex = 0;
            out.chase.notes.push_back( h );
        }
        if( start_ >= startPos && start_ < end ) {
            twEvent e;
            e.time = start_ - startPos; e.kind = twEventKind::NoteOn;
            e.channel = 0; e.key = key_; e.noteId = 0; e.value = vel_;
            out.events.push_back( e );
        }
        if( nEnd >= startPos && nEnd < end ) {
            twEvent e;
            e.time = nEnd - startPos; e.kind = twEventKind::NoteOff;
            e.channel = 0; e.key = key_; e.noteId = 0;
            out.events.push_back( e );
        }
        out.chase.sortNotes();
        out.sortEvents();
    }

private:
    std::int64_t start_, dur_;
    std::int16_t key_;
    double       vel_;
};

// One one-shot zone over every key, root 60, the layer id given. One-shot plays
// the layer to its end whatever the note-off says, at the root pitch for key 60.
std::string oneZoneProgram( std::uint32_t layerId )
{
    return "<NassauDigitalProgram version=\"1\" name=\"Q0\" nextId=\"" +
           std::to_string( layerId + 1 ) +
           "\"><zone id=\"1\" keyLo=\"0\" keyHi=\"127\" rootKey=\"60\" mode=\"oneshot\">"
           "<layer id=\"" + std::to_string( layerId ) +
           "\" src=\"test:q0\"/></zone></NassauDigitalProgram>";
}

// A deterministic stereo sample: L a sine, R the same sine at half level, so a
// fold is distinguishable from either channel. A SINE, because the core reads
// every layer through its band-limiting interpolator (a Kaiser-windowed sinc
// with its cutoff at 0.45 fs, nassau-digital nd_interp.h) even at the root
// pitch: a waveform with energy near Nyquist comes out filtered, so "the
// render equals the sample" holds only for band-limited material, and only to
// the filter's passband ripple.
struct Pcm {
    std::vector<float> l, r;
};
Pcm makePcm( std::int64_t frames, double hz, double amp )
{
    Pcm p;
    p.l.resize( (std::size_t)frames );
    p.r.resize( (std::size_t)frames );
    for( std::int64_t i = 0; i < frames; ++i ) {
        const float v = (float)( amp * std::sin( 2.0 * 3.14159265358979323846 * hz *
                                                 (double)i / 48000.0 ) );
        p.l[(std::size_t)i] = v;
        p.r[(std::size_t)i] = 0.5f * v;
    }
    return p;
}
std::shared_ptr<const twSamplerSample> toSample( const Pcm &p, std::uint64_t gen )
{
    const float *ch[2] = { p.l.data(), p.r.data() };
    std::string err;
    auto s = twSamplerSample::fromPlanar( 2, 48000.0, ch, (std::int64_t)p.l.size(), gen, &err );
    if( !s ) std::cerr << "  (sample refused: " << err << ")" << std::endl;
    return s;
}

std::vector<std::vector<float>> render( twPluginSlotProcessor &proc, int nCh, offset_t pos,
                                        length_t len, int rate )
{
    std::vector<std::vector<float>> in( (std::size_t)nCh, std::vector<float>( (std::size_t)len, 0.0f ) );
    std::vector<std::vector<float>> out( (std::size_t)nCh, std::vector<float>( (std::size_t)len, -99.0f ) );
    std::vector<const float *> inP( (std::size_t)nCh );
    std::vector<float *>       outP( (std::size_t)nCh );
    for( int c = 0; c < nCh; ++c ) {
        inP[(std::size_t)c]  = in[(std::size_t)c].data();
        outP[(std::size_t)c] = out[(std::size_t)c].data();
    }
    proc.render( inP.data(), outP.data(), len, pos, true, rate );
    return out;
}

double maxAbs( const std::vector<float> &v, std::size_t from, std::size_t to )
{
    double m = 0.0;
    for( std::size_t i = from; i < to && i < v.size(); ++i ) m = std::max( m, (double)std::fabs( v[i] ) );
    return m;
}

// max |out[i] - ref[i - offset]| over [from, to)
double maxErr( const std::vector<float> &out, const std::vector<float> &ref, std::size_t offset,
               std::size_t from, std::size_t to )
{
    double m = 0.0;
    for( std::size_t i = from; i < to; ++i ) {
        const double want = ( i >= offset && i - offset < ref.size() ) ? ref[i - offset] : 0.0;
        m = std::max( m, std::fabs( (double)out[i] - want ) );
    }
    return m;
}

twSamplerHost *hostOf( twPlugin &p ) { return dynamic_cast<twSamplerHost *>( &p ); }

// ---------------------------------------------------------------- A + B ---

void testRegistration()
{
    std::cout << "=== A/B: registration by call, and the capability ===" << std::endl;

    // A fresh registry with no search paths and no cache: only built-ins.
    twPluginRegistry reg;
    reg.setSearchPaths( {} );
    reg.setCachePath( "" );

    twPluginDescriptor d;
    check( !reg.findByUid( "tw", kNassauDigitalUid, d ),
           "an UNTOLD registry does not know tw.native.nassaudigital (no static-init registration)" );

    // Seed the list BEFORE registering: the registration must still show.
    const std::size_t before = reg.plugins().size();
    registerNassauDigital( reg );
    check( reg.findByUid( "tw", kNassauDigitalUid, d ), "registerNassauDigital() makes it resolvable" );
    check( d.isInstrument && d.acceptsNotes && d.io.audioInputs == 0 && d.io.audioOutputs == 2,
           "its descriptor is a 0 -> 2 instrument that accepts notes" );
    check( d.nOutBuses == 1 && d.outBusChannels.size() == 1 && d.outBusChannels[0] == 2,
           "it reports ONE stereo bus until Q8 (B7)" );
    check( d.name.find( "Smaragd" ) == std::string::npos, "its name is not a code name" );

    auto countIt = [&]() {
        int n = 0;
        for( const twPluginDescriptor &x : reg.plugins() )
            if( x.format == "tw" && x.uid == kNassauDigitalUid ) ++n;
        return n;
    };
    check( reg.plugins().size() == before + 1 && countIt() == 1,
           "an already-seeded list gains it once" );
    registerNassauDigital( reg );
    check( countIt() == 1, "a second registration replaces, never duplicates" );
    reg.rescan();
    check( countIt() == 1, "a rescan keeps it, once" );

    // A registry registered BEFORE its first seed lists it too.
    twPluginRegistry reg2;
    reg2.setSearchPaths( {} );
    reg2.setCachePath( "" );
    registerNassauDigital( reg2 );
    bool listed = false;
    for( const twPluginDescriptor &x : reg2.plugins() )
        if( x.uid == kNassauDigitalUid ) listed = true;
    check( listed, "a registration before the first seed is listed by the lazy seed" );

    std::unique_ptr<twPlugin> p = reg.instantiate( d );
    check( p != nullptr, "the registry instantiates it" );
    check( p && hostOf( *p ) != nullptr, "twSamplerHost is reached by capability (dynamic_cast)" );
    check( p && p->capabilities().isInstrument && p->capabilities().acceptsNotes,
           "the instance says it is an instrument" );

    for( const char *uid : { "tw.passthrough", "tw.native.303" } ) {
        twPluginDescriptor o;
        if( reg.findByUid( "tw", uid, o ) ) {
            std::unique_ptr<twPlugin> q = reg.instantiate( o );
            check( q && hostOf( *q ) == nullptr,
                   std::string( uid ) + " still instantiates, and is NOT a twSamplerHost" );
        }
    }
}

// --------------------------------------------------------------------- C ---

struct Rig {
    std::shared_ptr<twPluginSlotProcessor> proc;
    std::shared_ptr<twPluginInsert>        insert;
};

Rig makeRig( tw303aEnvironment &env, twPluginRegistry &reg, int nChannels )
{
    twPluginDescriptor d;
    reg.findByUid( "tw", kNassauDigitalUid, d );
    Rig r;
    r.proc = std::make_shared<twPluginSlotProcessor>(
        env, [&reg, d]() { return reg.instantiate( d ); }, d.io );
    r.proc->setChannelCount( (idx_t)nChannels );
    r.insert = std::make_shared<twPluginInsert>( env, r.proc );
    r.insert->init();
    return r;
}

void testRows( tw303aEnvironment &env, twPluginRegistry &reg )
{
    std::cout << "=== C: the generator rows ===" << std::endl;
    check( makeRig( env, reg, 2 ).proc->mode() == twPluginSlotMode::DirectGen, "2 channels: DirectGen" );
    check( makeRig( env, reg, 1 ).proc->mode() == twPluginSlotMode::GenFold, "1 channel: GenFold" );
    Rig six = makeRig( env, reg, 6 );
    check( six.proc->mode() == twPluginSlotMode::Transparent &&
               six.proc->state() == twPluginSlotState::Unsupported,
           "6 channels: no defined spread, Transparent + Unsupported (Q-7)" );
}

// ----------------------------------------------------------------- D + E ---

void testInstall( tw303aEnvironment &env, twPluginRegistry &reg )
{
    std::cout << "=== D/E: install through applyToInstances, read by process() ===" << std::endl;
    const int rate = env.getSRate();
    const std::uint32_t kLayer = 2;

    Rig r = makeRig( env, reg, 2 );
    r.proc->setEventSource( std::make_shared<OneNoteSource>( 1000, 48000, 60, 127.0 ) );

    // Before any install: the instrument has no program and must be silent.
    auto out = render( *r.proc, 2, 0, 8192, rate );
    check( maxAbs( out[0], 0, 8192 ) == 0.0 && maxAbs( out[1], 0, 8192 ) == 0.0,
           "with nothing installed the note is silent" );

    const Pcm a = makePcm( 20000, 440.0, 0.5 );
    auto progA  = twSamplerProgram::fromXml( oneZoneProgram( kLayer ) );
    auto sampA  = toSample( a, 1 );
    check( progA && sampA, "the program and the sample are accepted by the core" );
    if( !progA || !sampA ) return;

    const std::uint64_t epochBefore = r.insert->contentEpochNow();
    std::size_t seen = r.proc->applyToInstances( [&]( twPlugin &p ) {
        if( twSamplerHost *h = hostOf( p ) ) {
            h->setProgram( progA );
            h->setSample( kLayer, sampA );
            h->setRoutedOutputs( 0x5u );
        }
    } );
    check( seen == 1, "applyToInstances reaches the one generator instance" );
    check( r.insert->contentEpochNow() > epochBefore,
           "applyToInstances moves the insert's content epoch (a cached page cannot survive it)" );

    r.proc->forgetContinuity();
    out = render( *r.proc, 2, 0, 8192, rate );
    check( maxAbs( out[0], 0, 1000 ) == 0.0 && maxAbs( out[1], 0, 1000 ) == 0.0,
           "silent before the note at frame 1000" );
    const double eL = maxErr( out[0], a.l, 1000, 1000, 8192 );
    const double eR = maxErr( out[1], a.r, 1000, 1000, 8192 );
    std::cout << "       max |render - sample| after the note: L " << eL << ", R " << eR << std::endl;
    // The tolerance is the interpolator's passband ripple at 440 Hz, measured
    // (printed above), not a guess; a voice that played nothing, the wrong
    // sample, the wrong pitch or the wrong channel is off by the signal itself.
    check( eL < 2e-3 && eR < 1e-3, "after it, the sample plays at the root pitch, on both channels" );

    std::vector<twPlugin *> inst = r.proc->plugins();
    twSamplerHost *h = inst.empty() ? nullptr : hostOf( *inst[0] );
    if( check( h != nullptr, "the live instance is a twSamplerHost" ) ) {
        const twSamplerHostStats st = h->stats();
        check( st.processCalls > 0, "process() ran" );
        check( st.programSerial == progA->serial(), "process() ran on the installed program" );
        check( st.routedMask == 0x5u, "process() read the installed routed-outputs mask" );
        check( st.samplesInstalled == 1, "process() saw one installed sample" );
        check( inst[0]->tailFrames() >= 20000u - 1u,
               "a one-shot zone's tail covers the installed sample's length" );
    }

    // E. Contiguous renders: the note started under A; B arrives between two
    // pages; the sounding voice keeps A. The reference is ONE uninterrupted A
    // render — the core is block-size independent (its control grid counts from
    // the note-on), so two contiguous halves must equal it exactly.
    r.proc->forgetContinuity();
    const auto refA = render( *r.proc, 2, 0, 8192, rate );
    const Pcm b = makePcm( 20000, 660.0, 0.3 );
    auto sampB  = toSample( b, 2 );
    r.proc->forgetContinuity();
    auto first = render( *r.proc, 2, 0, 4096, rate );
    r.proc->applyToInstances( [&]( twPlugin &p ) {
        if( twSamplerHost *hh = hostOf( p ) ) hh->setSample( kLayer, sampB );
    } );
    auto second = render( *r.proc, 2, 4096, 4096, rate );
    bool keep = true;
    for( std::size_t i = 0; i < 4096; ++i ) {
        if( first[0][i] != refA[0][i] ) keep = false;
        if( second[0][i] != refA[0][4096 + i] || second[1][i] != refA[1][4096 + i] ) keep = false;
    }
    check( keep, "a voice keeps the buffer current at its note-on (DESIGN §6.1 rule 4): "
                 "the two halves equal one uninterrupted A render, bit for bit" );

    // ...and a NEW note (a fresh render from the top) plays B.
    r.proc->forgetContinuity();
    out = render( *r.proc, 2, 0, 8192, rate );
    const double eB = maxErr( out[0], b.l, 1000, 1000, 8192 );
    check( eB < 2e-3 && out[0] != refA[0], "the next note-on plays the new buffer" );

    if( h ) {
        // The core parks a replaced buffer until no one else holds it; the test
        // drops its own handle so the core's park is the last one.
        sampA.reset();
        int freed = 0;
        r.proc->applyToInstances( [&]( twPlugin &p ) {
            if( twSamplerHost *hh = hostOf( p ) ) freed += hh->collectRetired();
        } );
        check( freed >= 1, "collectRetired() frees the replaced buffer once no voice holds it" );
    }

    // Q0.q2 at engine level is the slot's job (the initializer); here: a
    // REBUILD re-instantiates, and without an initializer the new instance is
    // born empty — which is exactly why SPluginSlot needs one (invariant 4).
    r.proc->setChannelCount( 1 );
    r.proc->forgetContinuity();
    out = render( *r.proc, 1, 0, 8192, rate );
    check( maxAbs( out[0], 0, 8192 ) == 0.0,
           "a rebuild without an instance initializer forgets the install (why invariant 4 exists)" );
}

// --------------------------------------------------------------------- F ---

void testRace( tw303aEnvironment &env, twPluginRegistry &reg )
{
    std::cout << "=== F: installs racing renders land between process() calls ===" << std::endl;
    const int rate = env.getSRate();
    const std::uint32_t kLayer = 2;
    Rig r = makeRig( env, reg, 2 );
    r.proc->setEventSource( std::make_shared<OneNoteSource>( 1000, 48000, 60, 127.0 ) );
    const Pcm a = makePcm( 12000, 440.0, 0.5 ), b = makePcm( 12000, 660.0, 0.3 );
    auto prog = twSamplerProgram::fromXml( oneZoneProgram( kLayer ) );
    auto sa = toSample( a, 1 ), sb = toSample( b, 2 );
    r.proc->applyToInstances( [&]( twPlugin &p ) {
        if( twSamplerHost *h = hostOf( p ) ) { h->setProgram( prog ); h->setSample( kLayer, sa ); }
    } );

    // The two goldens, rendered alone.
    const auto goldA = render( *r.proc, 2, 0, 8192, rate );
    r.proc->applyToInstances( [&]( twPlugin &p ) {
        if( twSamplerHost *h = hostOf( p ) ) h->setSample( kLayer, sb );
    } );
    const auto goldB = render( *r.proc, 2, 0, 8192, rate );
    check( goldA[0] != goldB[0], "the two goldens differ" );

    std::atomic<bool> stop{ false };
    std::thread writer( [&]() {
        bool useB = false;
        while( !stop.load() ) {
            r.proc->applyToInstances( [&]( twPlugin &p ) {
                if( twSamplerHost *h = hostOf( p ) ) h->setSample( kLayer, useB ? sb : sa );
            } );
            useB = !useB;
            std::this_thread::yield();
        }
    } );
    int pagesA = 0, pagesB = 0, torn = 0;
    for( int i = 0; i < 300; ++i ) {
        auto out = render( *r.proc, 2, 0, 8192, rate );   // start != lastEnd: reset every time
        if( out == goldA ) ++pagesA; else if( out == goldB ) ++pagesB; else ++torn;
    }
    stop = true;
    writer.join();
    std::cout << "       300 pages: " << pagesA << " A, " << pagesB << " B, " << torn << " neither" << std::endl;
    check( torn == 0, "every page is exactly the A render or exactly the B render" );
}

// --------------------------------------------------------------------- G ---

void testStateBlob( tw303aEnvironment &env, twPluginRegistry &reg )
{
    std::cout << "=== G: the state blob holds no sample ===" << std::endl;
    Rig r = makeRig( env, reg, 2 );
    twPlugin *p = r.proc->plugin();
    if( !check( p != nullptr, "an instance exists" ) ) return;
    const std::vector<std::uint8_t> before = p->saveState();

    // 10 MiB of PCM: 1 310 720 stereo float frames.
    const std::int64_t frames = 1310720;
    const Pcm big = makePcm( frames, 440.0, 0.25 );
    auto prog = twSamplerProgram::fromXml( oneZoneProgram( 2 ) );
    auto s    = toSample( big, 7 );
    const std::size_t pcmBytes = (std::size_t)frames * 2u * sizeof( float );
    r.proc->applyToInstances( [&]( twPlugin &pl ) {
        if( twSamplerHost *h = hostOf( pl ) ) { h->setProgram( prog ); h->setSample( 2, s ); }
    } );
    const std::vector<std::uint8_t> after = p->saveState();
    std::cout << "       blob " << before.size() << " B before, " << after.size()
              << " B after a " << pcmBytes << " B sample" << std::endl;
    check( before.size() == 8 && after == before, "the blob is the 8-byte header before and after" );
    check( after.size() >= 4 && std::memcmp( after.data(), "TWND", 4 ) == 0, "it carries the TWND magic" );
    check( p->loadState( after ), "the instrument reads its own blob back" );
    std::vector<std::uint8_t> newer = after;
    newer[4] = 2;   // version 2
    check( !p->loadState( newer ), "a NEWER version is refused, never guessed at" );
}

}  // namespace

int main()
{
    tw303aEnvironment env;
    env.setSRate( 48000 );

    testRegistration();

    twPluginRegistry reg;
    reg.setSearchPaths( {} );
    reg.setCachePath( "" );
    registerNassauDigital( reg );

    testRows( env, reg );
    testInstall( env, reg );
    testRace( env, reg );
    testStateBlob( env, reg );

    if( gFailures ) {
        std::cerr << gFailures << " check(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "sampler_test: all checks passed" << std::endl;
    return 0;
}
