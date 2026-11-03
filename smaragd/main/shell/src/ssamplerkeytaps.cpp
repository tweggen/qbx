#include "app/shell/ssamplerkeytaps.h"

#include <QTimer>

#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/sampler/ssamplerprogram.h"
#include "app/objects/track/strack.h"
#include "app/shell/sapplication.h"
#include "app/shell/smidiinputhub.h"
#include "app/shell/ssamplerwiring.h"

#include "tw/core/twlog.h"

namespace {

// Every track under `container`, depth first.
void collectTracks( SObject *container, std::vector<STrack *> &out )
{
    if( !container ) return;
    for( int i = 0; i < container->childCount(); ++i ) {
        SLink *lk = container->childAt( i );
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child->isLane() ) continue;
        if( STrack *t = dynamic_cast<STrack *>( child ) ) out.push_back( t );
        collectTracks( child, out );
    }
}

}  // namespace

SSamplerKeyTaps::SSamplerKeyTaps( SApplication *app, QObject *parent )
    : QObject( parent ), app_( app )
{
    // 10 ms: the key a recording lands on is the one played before the take
    // stopped, which is seconds away; the interval only bounds how stale the
    // reading can be when a stop follows a key at once.
    timer_ = new QTimer( this );
    timer_->setInterval( 10 );
    connect( timer_, &QTimer::timeout, this, &SSamplerKeyTaps::poll );
    if( app_ )
        connect( app_, &SApplication::liveLanesMayHaveChanged, this, &SSamplerKeyTaps::sync );
}

SSamplerKeyTaps::~SSamplerKeyTaps()
{
    releaseAll_();
}

void SSamplerKeyTaps::releaseAll_()
{
    for( Tap &t : taps_ )
        if( t.fanout && t.sink ) t.fanout->release( t.sink );
    taps_.clear();
    if( timer_ ) timer_->stop();
}

void SSamplerKeyTaps::projectChanged( SProject *project )
{
    if( project == project_ ) return;
    if( project_ ) disconnect( project_, nullptr, this, nullptr );
    releaseAll_();
    project_ = project;
    if( project_ )
        connect( project_, &SProject::arrangementChanged, this, &SSamplerKeyTaps::sync );
    sync();
}

void SSamplerKeyTaps::sync()
{
    SMidiInputHub *hub = app_ ? app_->midiInputHub() : nullptr;
    std::vector<Tap> want;
    if( project_ && hub ) {
        std::vector<STrack *> tracks;
        // check_root_resolution: master -- the walk covers the master AND every arrangement root, by name, below
        collectTracks( project_->masterRoot(), tracks );
        for( const QString &n : project_->arrangementNames() )
            collectTracks( project_->arrangement( n ), tracks );
        for( STrack *tr : tracks ) {
            if( !tr->hasMidiTrackInput() ) continue;
            for( SSamplerProgram *p : ssamplerwiring::programsOf( tr ) ) {
                Tap t;
                t.program = p;
                t.port    = tr->trackInputMidiPort();
                t.channel = tr->trackInputMidiChannel();
                want.push_back( t );
            }
        }
    }

    // Keep a tap that is still wanted unchanged (its ring keeps what arrived);
    // release the rest; acquire what is new.
    std::vector<Tap> next;
    for( Tap &have : taps_ ) {
        bool keep = false;
        for( Tap &w : want )
            if( w.program == have.program && w.port == have.port && w.channel == have.channel &&
                !w.sink ) {
                w.sink = have.sink;   // mark as covered
                keep   = true;
                break;
            }
        if( keep && have.program ) next.push_back( have );
        else if( have.fanout && have.sink ) have.fanout->release( have.sink );
    }
    for( Tap &w : want ) {
        if( w.sink ) continue;    // covered by a kept tap
        w.fanout = hub->fanoutFor( w.port );
        if( !w.fanout ) continue;
        const std::uint16_t mask = ( w.channel < 0 ) ? (std::uint16_t) 0xFFFF
                                                     : (std::uint16_t)( 1u << w.channel );
        w.sink = w.fanout->acquire( mask, "NassauDigital key tap" );
        if( !w.sink ) {
            TW_LOGW( "shell", "[SAMPLER] no free MIDI input sink on port '%s' for a "
                              "NassauDigital key tap",
                     w.port.toStdString().c_str() );
            continue;
        }
        next.push_back( w );
    }
    taps_ = std::move( next );
    if( taps_.empty() ) timer_->stop();
    else if( !timer_->isActive() ) timer_->start();
}

void SSamplerKeyTaps::poll()
{
    for( Tap &t : taps_ ) {
        if( !t.sink ) continue;
        audio::MidiInMessage m;
        while( t.sink->pop( m ) ) {
            if( !t.program ) continue;          // drain a dead tap's ring anyway
            if( m.status() == 0x90 && m.size >= 3 && m.bytes[2] > 0 ) {
                t.program->setLastKey( m.bytes[1] & 0x7F );
                // DESIGN §13 (2026-11-03): the note selects the pad on its key.
                t.program->selectPadByKey( m.bytes[1] & 0x7F );
            }
        }
    }
}
