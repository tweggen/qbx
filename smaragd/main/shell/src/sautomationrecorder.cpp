#include "app/shell/sautomationrecorder.h"

#include "app/shell/sapplication.h"
#include "app/objects/track/sautomationactions.h"
#include "app/model/sobject.h"

#include <QDebug>
#include <algorithm>

SAutomationRecorder::SAutomationRecorder( QObject *parent )
    : QObject( parent )
{
}

SAutomationRecorder::~SAutomationRecorder() = default;

SAutomationMode SAutomationRecorder::modeOf( const Target &t )
{
    SProject *proj = SApplication::app().getCurrentProject();
    if( !proj ) return SAutomationMode::Off;
    sautomation::OwnerRef o = sautomation::resolveOwner(
        proj, t.ownerPath, t.target, t.slotIndex, t.take );
    if( !o.valid() ) return SAutomationMode::Off;
    SAutomationLane *lane = o.owner->automationLane( t.target );
    return lane ? lane->mode() : SAutomationMode::Off;
}

void SAutomationRecorder::transportStarted( offset_t pos )
{
    // A pass that is still open when the transport restarts belongs to the
    // previous run: commit it rather than letting it span a gap in time that
    // was never played.
    if( active_ ) commit_( lastFrame_ );
    transportRunning_ = true;
    passStart_ = pos;
}

void SAutomationRecorder::transportStopped( offset_t pos )
{
    transportRunning_ = false;
    if( active_ ) commit_( pos );
}

bool SAutomationRecorder::isRecording( const Target &t ) const
{
    return active_ && target_ == t;
}

bool SAutomationRecorder::writeTick( const Target &t, double value,
                                     offset_t frame )
{
    const SAutomationMode m = modeOf( t );
    if( !isRecordMode( m ) ) return false;

    // A tick aimed at a DIFFERENT lane ends the pass that is open: two
    // simultaneous passes would each own part of one undo step.
    if( active_ && !( target_ == t ) ) commit_( lastFrame_ );

    if( !active_ ) {
        active_     = true;
        released_   = false;
        target_     = t;
        mode_       = m;
        firstFrame_ = frame;
        // Write overwrites the whole transport run, so its window opens where
        // the run did — but never AFTER the punch-in (a punch-in before the
        // transport started, or a transportStarted we were never told about,
        // must not produce an inverted window).
        if( !transportRunning_ || passStart_ > frame ) passStart_ = frame;
        pts_.clear();
    }

    SAutomationPoint p;
    p.frame = frame;
    p.value = value;
    p.shape = ( SParamRef::parse( t.target ).prop == QLatin1String( "Muted" ) )
                  ? twCurveShape::Step : twCurveShape::Linear;
    // A tick that lands on a frame already written REPLACES it: the pointer
    // moved twice inside one frame, and a lane holds one point per frame.
    if( !pts_.empty() && pts_.back().frame >= frame ) {
        while( !pts_.empty() && pts_.back().frame >= frame ) pts_.pop_back();
    }
    pts_.push_back( p );
    lastFrame_ = frame;
    lastValue_ = value;
    return true;
}

void SAutomationRecorder::releaseControl()
{
    if( !active_ ) return;
    released_ = true;
    // Touch lets go of the lane the instant the control is released — that IS
    // the mode. Latch and Write keep the pass open and hold the last value.
    if( mode_ == SAutomationMode::Touch ) commit_( lastFrame_ );
}

void SAutomationRecorder::commit_( offset_t stopFrame )
{
    const bool wasActive = active_;
    Target t = target_;
    const SAutomationMode m = mode_;
    std::vector<SAutomationPoint> pts;
    pts.swap( pts_ );
    const offset_t first = firstFrame_;
    const offset_t start = passStart_;
    const double   held  = lastValue_;

    active_   = false;
    released_ = false;

    if( !wasActive || pts.empty() ) return;

    offset_t from = first;
    if( m == SAutomationMode::Write && start < from ) {
        // Overwrite the pass: the value the hand arrived with is written back
        // to where the transport started, so the whole run reads as the pass.
        from = start;
        SAutomationPoint anchor = pts.front();
        anchor.frame = from;
        pts.insert( pts.begin(), anchor );
    }

    offset_t last = pts.back().frame;
    if( m != SAutomationMode::Touch && stopFrame > last ) {
        // Latch / Write hold the last value to the transport stop. One point,
        // not a stream of identical ticks.
        SAutomationPoint hold = pts.back();
        hold.frame = stopFrame;
        hold.value = held;
        pts.push_back( hold );
        last = stopFrame;
    }

    // `set-automation-points` drops anything outside [from, to), so `to` has to
    // clear the last point rather than sit on it.
    const offset_t to = last + 1;

    SApplication::app().submitAction(
        new SSetAutomationPointsAction( t.ownerPath, t.target, from, to,
                                        std::move( pts ), t.slotIndex,
                                        t.take ) );
}
