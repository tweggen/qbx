#ifndef _SRECORDPLACEMENT_H_
#define _SRECORDPLACEMENT_H_

#include <cstdint>

#include "tw/playback/twliveclock.h"

/**
 * THE PLACEMENT CONVERSION (proposal 21 L3b, design D6) — named once, coded
 * once, and used by the recorder, by `assert-recorded-clip` and by nothing
 * else. Every number below is in PROJECT frames.
 *
 *     placementFrame(k) = P0 + k
 *                            - inputLatencyProj
 *                            - outputLatencyProj
 *                            + userOffsetProj
 *
 * `P0` is the project frame that capture frame 0's HOST TIME maps to through
 * the live clock's anchor — never "the locator when the button was pressed".
 * The anchor is `twEngineClock::read()`'s `{deliveredFrame, hostNs}`, i.e. the
 * ENGINE-owned atomic stamped in the render callback, which already carries
 * `SMidiOutPump`'s publish-lag correction (`twliveclock.h`: deliveredFrame is
 * `currentPosition() - bufferFrames`). Reading `SApplication`'s locator
 * instead would put a UI-thread value on the critical path and re-derive a
 * correction that is already applied.
 *
 * THE SIGN, so it is not re-argued (design D6's derivation): the performer
 * plays to what they HEAR, which is the engine position emitted
 * `outputLatency` earlier; the microphone's sample reaches the ADC
 * `inputLatency` before it is delivered. So the musical moment of capture
 * frame k is `positionDelivered(t_arrival(k)) - outLat - inLat` — what
 * REAPER's "use audio driver reported latency" does.
 *
 * `userOffsetProj` is the term the design writes with a PLUS, and
 * `SSettings::recordingOffsetMs` is the knob behind it. That setting's sign
 * convention is the app-wide one (37 P7's `midi/offsetMs`): **POSITIVE means
 * EARLIER**, i.e. "the driver under-reports; compensate more". So
 * `setUserOffsetMs(+20)` at 48 kHz stores `userOffsetProj = -960` and the
 * placed clip moves 960 frames earlier. The negation lives in ONE setter,
 * with this comment beside it, rather than being spread over call sites.
 *
 * RECORDING FROM A STOPPED TRANSPORT. Capture frame 0's host time precedes the
 * first publication (the readahead primes before the RT publishes), so no
 * anchor exists when the capture starts. The bridge stamps the host time of
 * its first frame (`CaptureBridge::captureStartHostNs()`) and the recorder
 * applies the mapping RETROSPECTIVELY as soon as an anchor appears —
 * backward extrapolation on the same clock, which is exact because the clock
 * is a linear function of host time at the project rate. Frames whose
 * placement lands before the transport start are TRIMMED (see
 * `SAudioRecorder`); a Cubase-style catch range is not implemented.
 *
 * DEVICE FRAMES vs PROJECT FRAMES. The two latencies come from the driver in
 * DEVICE frames at the DEVICE rate. They are scaled here by exactly the same
 * `projectRate / deviceRate` ratio `SApplication::meterLatencyFrames()` uses
 * (proposal 34); skipping it is a ~9 % error for a 44.1 k project on a 48 k
 * device.
 */
struct SRecordPlacement
{
    std::int64_t p0                 = 0;
    std::int64_t inputLatencyProj   = 0;
    std::int64_t outputLatencyProj  = 0;
    std::int64_t userOffsetProj     = 0;
    bool         anchored           = false;

    /// Design D6, verbatim.
    std::int64_t placementFrame( std::int64_t k ) const
    {
        return p0 + k - inputLatencyProj - outputLatencyProj + userOffsetProj;
    }

    /// `placementFrame(k) - p0 - k` — everything the conversion moved the
    /// audio by, independent of when the anchor happened to be taken. This is
    /// the number a gate can hold to a closed form.
    std::int64_t compensationFrames() const
    {
        return -inputLatencyProj - outputLatencyProj + userOffsetProj;
    }

    /// See the class comment: POSITIVE ms == EARLIER.
    void setUserOffsetMs( double ms, double projectRate )
    {
        userOffsetProj = -(std::int64_t) ( ms * projectRate / 1000.0
                                           + ( ms >= 0.0 ? 0.5 : -0.5 ) );
    }
};

namespace srecord {

/**
 * The project frame at host time `hostNs`, extrapolated from an engine-clock
 * anchor at the project rate. BACKWARD extrapolation (hostNs < anchor.hostNs)
 * is the ordinary case for a record start from a stopped transport and is not
 * special-cased: the clock is linear in host time, so one expression covers
 * both directions.
 */
inline std::int64_t projectFrameAtHostNs( const twEnginePosition &anchor,
                                          std::int64_t hostNs,
                                          double projectRate )
{
    const double dNs = (double) ( hostNs - anchor.hostNs );
    const double dFr = dNs * projectRate / 1e9;
    return anchor.deliveredFrame
         + (std::int64_t) ( dFr + ( dFr >= 0.0 ? 0.5 : -0.5 ) );
}

}  // namespace srecord

#endif // _SRECORDPLACEMENT_H_
