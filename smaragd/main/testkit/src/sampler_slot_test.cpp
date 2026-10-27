// sampler_slot_test — proposal 56 Q0.4 and Q0.q2, on a REAL SProject / STrack /
// SPluginSlot against a StubAppContext (the project_channels_test shape): no
// display, no audio device, no render session.
//
//   Q0.4  THE SAVED PROJECT HOLDS THE REFERENCE ONLY. A project with a
//         NassauDigital slot is serialized, a 10 MiB sample is installed into
//         the slot (through the slot's instance initializer and
//         applyToInstances, the only routes Q0 has), and the project is
//         serialized again: the document grows by less than 1 KiB. (It grows
//         by nothing at all in Q0, where the program has no model yet; the AC's
//         1 KiB is what Q1's program element will have to fit under too.)
//
//   Q0.q2 A REBUILD KEEPS THE PROGRAM AND THE SAMPLES (invariant 4). The slot
//         renders a note on a 2-channel project; the project goes to 1 channel,
//         which re-derives the mapping and RE-INSTANTIATES the plugin; the
//         render after it is the GenFold of the render before it — exactly,
//         0.5f * (L + R) per frame. Without the initializer the new instance
//         would be born empty and render silence (sampler_test shows that half).
//
// The slot's processor is driven directly (render() with planar buffers), as
// plugins_test drives one: what is under test is the slot's factory and its
// initializer, not the freeze machinery above the insert.

#include "app/model/sappcontext.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"

#include "tw/events/tweventsource.h"
#include "tw/graph/tw303aenv.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplerregistry.h"
#include "tw/sampler/twsamplertypes.h"

#include <QApplication>
#include <QString>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check( bool ok, const char *what )
{
    if( ok ) { std::printf( "ok   %s\n", what ); return; }
    std::printf( "FAIL %s\n", what );
    ++g_failures;
}

class StubAppContext : public SAppContext {
public:
    SProject *getCurrentProject() const override { return nullptr; }
    tw303aEnvironment *get303aEnvironment() const override { return &env_; }
    void rewireSpeaker() override {}
    bool isSLinkSelected( SLink * ) const override { return false; }
    void setSelectionFromPaths( const QList<QList<int>> & ) override {}
    void addSelectionFromPaths( const QList<QList<int>> & ) override {}
    void removeSelectionFromPaths( const QList<QList<int>> & ) override {}
    void toggleSelectionFromPaths( const QList<QList<int>> & ) override {}
    QList<QList<int>> getCurrentSelectionPaths() const override { return {}; }
    QString testOutputDir() const override { return QString(); }
    bool ensureOutputDirExists() const override { return false; }
    bool startRender( const audio::RenderParams &, QString * ) override { return false; }
    bool isRenderingActive() const override { return false; }
    void setPlaybackRunning( bool ) override {}
    offset_t getGlobalLocatorPos() const override { return 0; }

    tw303aEnvironment &env() const { return env_; }

private:
    mutable tw303aEnvironment env_;
};

class OneNoteSource : public twEventSource {
public:
    void collect( std::int64_t startPos, std::int64_t len, twEventBlock &out ) const override
    {
        out.clear();
        const std::int64_t start = 1000, dur = 48000;
        if( start >= startPos && start < startPos + len ) {
            twEvent e;
            e.time = start - startPos; e.kind = twEventKind::NoteOn;
            e.channel = 0; e.key = 60; e.noteId = 0; e.value = 127.0;
            out.events.push_back( e );
        }
        if( start < startPos && start + dur > startPos ) {
            twHeldNote h;
            h.key = 60; h.channel = 0; h.velocity = 127.0; h.noteId = 0;
            h.start = start; h.duration = dur; h.srcIndex = 0;
            out.chase.notes.push_back( h );
        }
        out.sortEvents();
    }
};

QString serialized( SProject &p )
{
    QString s;
    QTextStream ts( &s );
    p.serialize( ts );
    ts.flush();
    return s;
}

std::vector<std::vector<float>> renderSlot( SPluginSlot &slot, int nCh, std::int64_t len, int rate )
{
    std::vector<std::vector<float>> in( (std::size_t)nCh, std::vector<float>( (std::size_t)len, 0.0f ) );
    std::vector<std::vector<float>> out( (std::size_t)nCh, std::vector<float>( (std::size_t)len, -9.0f ) );
    std::vector<const float *> ip;
    std::vector<float *>       op;
    for( int c = 0; c < nCh; ++c ) {
        ip.push_back( in[(std::size_t)c].data() );
        op.push_back( out[(std::size_t)c].data() );
    }
    slot.getProcessor()->forgetContinuity();
    slot.getProcessor()->render( ip.data(), op.data(), (length_t)len, 0, true, rate );
    return out;
}

double rms( const std::vector<float> &v, std::size_t from, std::size_t to )
{
    double s = 0.0;
    for( std::size_t i = from; i < to; ++i ) s += (double)v[i] * v[i];
    return to > from ? std::sqrt( s / (double)( to - from ) ) : 0.0;
}

// A stereo sine pair, L at twice R's level, so the fold is neither channel.
std::shared_ptr<const audio::twSamplerSample> sineSample( std::int64_t frames, std::uint64_t gen )
{
    std::vector<float> l( (std::size_t)frames ), r( (std::size_t)frames );
    for( std::int64_t i = 0; i < frames; ++i ) {
        l[(std::size_t)i] = (float)( 0.5 * std::sin( 2.0 * 3.14159265358979323846 * 440.0 * (double)i / 48000.0 ) );
        r[(std::size_t)i] = 0.5f * l[(std::size_t)i];
    }
    const float *ch[2] = { l.data(), r.data() };
    return audio::twSamplerSample::fromPlanar( 2, 48000.0, ch, frames, gen );
}

}  // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );
    StubAppContext ctx;
    SAppContext::setInstance( &ctx );
    ctx.env().setSRate( 48000 );
    const int rate = 48000;

    audio::registerNassauDigital( audio::pluginRegistry() );

    std::unique_ptr<SProject> project( new SProject );
    project->setChannels( 2 );
    SStdMixer *mixer = new SStdMixer( project.get() );
    project->setRootComponent( mixer );
    STrack *track = new STrack( project.get() );
    mixer->insertTrack( *track );

    audio::twPluginDescriptor desc;
    check( audio::pluginRegistry().findByUid( "tw", audio::kNassauDigitalUid, desc ),
           "the registry resolves tw.native.nassaudigital" );
    SPluginSlot *slot = new SPluginSlot( project.get(), desc );
    // Exactly insert-plugin's parenting: the SLink is built unparented, then
    // setParent() fires slotInserted -> STrack::onPluginSlotInserted ->
    // setChannelCount(), which instantiates the plugin.
    SLink *link = new SLink( *slot, nullptr );
    link->setParent( track->getPluginChain() );
    check( track->instrumentSlot() == slot, "the slot is the track's instrument (slot 0)" );
    check( slot->getSlotMode() == audio::twPluginSlotMode::DirectGen, "DirectGen on 2 channels" );

    // ------------------------------------------------------------- Q0.4 --
    const QString before = serialized( *project );

    const std::int64_t bigFrames = 1310720;   // 1 310 720 x 2 ch x 4 B = 10 MiB
    auto program = audio::twSamplerProgram::fromXml(
        "<NassauDigitalProgram version=\"1\" name=\"Q0\" nextId=\"4\"><zone id=\"3\" keyLo=\"0\" "
        "keyHi=\"127\" rootKey=\"60\" mode=\"oneshot\"><layer id=\"2\" src=\"test:q0\"/></zone>"
        "</NassauDigitalProgram>" );
    auto big = sineSample( bigFrames, 1 );
    check( program && big, "the core accepts the program and the 10 MiB sample" );
    auto install = [program]( std::shared_ptr<const audio::twSamplerSample> s ) {
        return [program, s]( audio::twPlugin &p ) {
            if( auto *h = dynamic_cast<audio::twSamplerHost *>( &p ) ) {
                h->setProgram( program );
                h->setSample( 2, s );
            }
        };
    };
    slot->setInstanceInitializer( install( big ) );
    const std::size_t n = slot->applyToInstances( install( big ) );
    check( n == 1, "applyToInstances reached the slot's instance" );

    const QString after = serialized( *project );
    const long long growth = (long long)after.toUtf8().size() - (long long)before.toUtf8().size();
    std::printf( "     project document: %lld B before, %lld B after a %lld B sample: growth %lld B\n",
                 (long long)before.toUtf8().size(), (long long)after.toUtf8().size(),
                 (long long)( bigFrames * 2 * 4 ), growth );
    // The measurement is only worth something if the slot IS in the document,
    // with its state chunk: the uid, and the 8-byte 'TWND' header in base64.
    check( after.contains( QStringLiteral( "tw.native.nassaudigital" ) ),
           "the saved project carries the NassauDigital slot" );
    check( after.contains( QStringLiteral( "VFdORAEAAAA=" ) ),
           "...and its state chunk, the 8-byte TWND header and nothing else" );
    check( growth < 1024, "Q0.4: the project file grows by less than 1 KB when a 10 MB sample is added" );

    // ------------------------------------------------------------ Q0.q2 --
    // A short sample for the render half: the fold is per frame, the length is
    // irrelevant, and 10 MiB through the sinc reader twice is time for nothing.
    auto shortS = sineSample( 30000, 2 );
    slot->setInstanceInitializer( install( shortS ) );
    slot->applyToInstances( install( shortS ) );
    slot->setEventSource( std::make_shared<OneNoteSource>() );

    const auto stereo = renderSlot( *slot, 2, 16384, rate );
    check( rms( stereo[0], 0, 1000 ) == 0.0, "silent before the note at frame 1000" );
    check( rms( stereo[0], 1000, 16384 ) > 0.3 && rms( stereo[1], 1000, 16384 ) > 0.15,
           "the stereo render carries the sample on both channels" );

    project->setChannels( 1 );   // STrack::setChannels -> SPluginSlot::setChannelCount: a REBUILD
    check( slot->getSlotMode() == audio::twPluginSlotMode::GenFold, "GenFold on 1 channel" );
    // The track re-applies its own (empty) feed on a width change; the note
    // comes from this test's source, so hand it over again.
    slot->setEventSource( std::make_shared<OneNoteSource>() );
    const auto mono = renderSlot( *slot, 1, 16384, rate );

    bool fold = true;
    double worst = 0.0;
    for( std::size_t i = 0; i < 16384; ++i ) {
        const float want = 0.5f * ( stereo[0][i] + stereo[1][i] );
        worst = std::max( worst, (double)std::fabs( mono[0][i] - want ) );
        if( mono[0][i] != want ) fold = false;
    }
    std::printf( "     mono vs 0.5*(L+R): max |diff| %g over 16384 frames; mono rms %g\n", worst,
                 rms( mono[0], 1000, 16384 ) );
    check( fold, "Q0.q2: after 2 -> 1 channels the render is EXACTLY the GenFold of the render before" );
    check( rms( mono[0], 1000, 16384 ) > 0.2, "...and it is not silence: the new instance was born current" );

    if( g_failures ) {
        std::printf( "\n%d check(s) FAILED\n", g_failures );
        return 1;
    }
    std::printf( "\nall checks passed\n" );
    return 0;
}
