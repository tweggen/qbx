#ifndef _SPLAYHEADCLOCK_H_
#define _SPLAYHEADCLOCK_H_

#include <QtGlobal>

/**
 * SPlayheadClock - HOST TIME <-> PROJECT FRAME, on the published playhead
 * (proposal 37 P7b's anchor discipline, extracted for proposal 21 L4).
 *
 * ONE clock, two consumers: `SMidiOutPump` asks "what host time is project
 * frame F heard at?" to schedule a message, and `SMidiRecorder` asks the
 * inverse - "what project frame was being heard when this byte arrived?" - to
 * place a recorded note. They are the same function read in two directions,
 * and a second implementation of it would be a second set of corrections to
 * keep in step. Everything below was `SMidiOutPump`'s private state and is
 * moved verbatim, not re-derived.
 *
 * THE ANCHOR is `(anchorAbs, anchorNs)`: the wrap-counted project frame
 * `anchorAbs` will be HEARD at host time `anchorNs`, and between anchors the
 * position is extrapolated linearly at the PROJECT rate. Three things about it
 * are load-bearing and each of them cost a measurement to find:
 *
 *  1. IT IS RE-TAKEN ON EVERY POSITION *PUBLICATION*, NOT EVERY POSITION
 *     *CHANGE*. `twSpeaker` defers the device start until the readahead is
 *     primed, so between the transport start and the first callback the
 *     playhead sits still at the locator. Anchoring on a change would either
 *     hang a due time on a clock that is not running yet - measured: the first
 *     note of a run went out 59 ms EARLY - or wait for the second callback and
 *     lose the first buffer of events with it. `SApplication::locatorPublishSeq()`
 *     is the counter the RT thread bumps beside the position store.
 *  2. THE PUBLISH LAG. `twSpeaker` publishes `engine->currentPosition()` AFTER
 *     the pull, so the value seen at `now` means "everything up to P has been
 *     HANDED to the device": the frame just delivered is `P - bufferFrames`.
 *     ~21 ms at 1024 frames / 48 kHz.
 *  3. THE DEVICE LATENCY. Audio handed over now is HEARD one output latency
 *     later. `SApplication::meterLatencyFrames()` is passed in because it
 *     already converts DEVICE frames at the DEVICE rate into PROJECT frames
 *     (proposal 34) - the ~9 % error at 44.1 kHz on a 48 kHz device.
 *
 * THE FIRST ANCHOR OF A RUN IS GUARDED. A locate is published by the UI thread
 * immediately, but the RT thread can still deliver one block against the OLD
 * position before the engine's seek lands. Anchoring the first time on such a
 * publication would put a whole window's due times in the past and flush it at
 * once, so the first anchor only accepts a position within one second of where
 * the run started; the publication is NOT consumed, and the next one is
 * retried. Every later anchor is unconditional - by then the playhead is the
 * authority on where playback actually is.
 *
 * `iterFrames` is the caller's loop bookkeeping (`iteration * cycleLength`),
 * folded in so `anchorAbs` and every frame this class returns are WRAP-COUNTED
 * and monotone. A consumer that does not cycle passes 0.
 *
 * Plain value type: no Qt object, no thread of its own, no state but the
 * anchor. Main thread only, like both its consumers.
 */
class SPlayheadClock
{
public:
    static constexpr qint64 kNsPerSec = 1000000000LL;

    /** Frames -> nanoseconds at the PROJECT rate (the locator's domain). */
    static qint64 framesToNs( qint64 frames, qint64 rate )
    {
        if( rate <= 0 ) return 0;
        return (qint64) ( (double) frames * (double) kNsPerSec / (double) rate );
    }

    static qint64 nsToFrames( qint64 ns, qint64 rate )
    {
        if( rate <= 0 ) return 0;
        return (qint64) ( (double) ns * (double) rate / (double) kNsPerSec );
    }

    /**
     * A transport run begins at `startPos`. `publishSeq` is the publication
     * counter AS OF NOW, so the first anchor waits for a publication that
     * happened after this run began: the counter is process-global and
     * monotone, so zero would be wrong on the second run of a process and
     * would anchor on a clock that has not started.
     */
    void beginRun( qint64 startPos, quint64 publishSeq )
    {
        haveAnchor_ = false;
        runStartPos_ = startPos;
        lastPublishSeq_ = publishSeq;
        anchorAbs_ = 0;
        anchorNs_  = 0;
    }

    /**
     * Offer this tick's observation of the playhead. Returns whether an anchor
     * is available afterwards - so a caller's whole guard is
     * `if( !clock.observe(...) ) return;`.
     *
     * `pos` is the published locator, `nowNs` the host time it was read at
     * (`MidiOutScheduler::hostNowNs()` domain, which is also the domain the
     * audio capture backend stamps its block log with and the MIDI input rings
     * stamp their messages with - that shared clock is what makes any of this
     * comparable).
     */
    bool observe( qint64 nowNs, qint64 pos, quint64 publishSeq, qint64 rate,
                  qint64 bufferFramesProject, qint64 outputLatencyFrames,
                  qint64 iterFrames )
    {
        if( publishSeq != lastPublishSeq_ ) {
            if( !haveAnchor_ && qAbs( pos - runStartPos_ ) > rate )
                return false;          // NOT consumed: retry on the next one
            lastPublishSeq_ = publishSeq;
            anchorAbs_  = pos - bufferFramesProject + iterFrames;
            anchorNs_   = nowNs + framesToNs( outputLatencyFrames, rate );
            haveAnchor_ = true;
        }
        return haveAnchor_;
    }

    bool   hasAnchor() const { return haveAnchor_; }
    qint64 anchorAbs() const { return anchorAbs_; }
    qint64 anchorNs()  const { return anchorNs_; }
    qint64 runStartPos() const { return runStartPos_; }

    /** The host time the wrap-counted project frame `absFrame` is HEARD at. */
    qint64 hostNsForFrame( qint64 absFrame, qint64 rate ) const
    { return anchorNs_ + framesToNs( absFrame - anchorAbs_, rate ); }

    /**
     * The inverse: the wrap-counted project frame being heard at `hostNs`.
     * BACKWARD extrapolation (a host time before the anchor was taken) is the
     * ordinary case for a MIDI byte that arrived before the RT published
     * anything, and is not special-cased - the clock is linear in host time,
     * so one expression covers both directions (the same argument
     * `srecord::projectFrameAtHostNs` makes for the audio recorder).
     */
    qint64 frameAtHostNs( qint64 hostNs, qint64 rate ) const
    { return anchorAbs_ + nsToFrames( hostNs - anchorNs_, rate ); }

private:
    bool    haveAnchor_ = false;
    qint64  anchorAbs_  = 0;
    qint64  anchorNs_   = 0;
    qint64  runStartPos_ = 0;
    quint64 lastPublishSeq_ = 0;
};

#endif // _SPLAYHEADCLOCK_H_
