#ifndef _SLIVETESTACTIONS_H_
#define _SLIVETESTACTIONS_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"

/**
 * The live-lane assertions (proposal 21 L1b, design D9 / section 7).
 *
 * THE MEASUREMENT IS INDEPENDENT OF THE THING MEASURED, which is the same
 * discipline the MIDI-out assertions follow and the only reason any of these
 * are worth anything: the INPUT is a WAV the test wrote (replayed by
 * `FileAudioInput` through a real capture thread and ring), the OUTPUT is the
 * audio capture backend's own recording of what the device was handed, and
 * nothing in between is asked what it thinks it did.
 */

/**
 * `assert-monitor-latency` - how far behind the input the monitored output is.
 *
 * By CROSS-CORRELATION, deliberately, not by the position decoder: the decoder
 * resolves 4096-frame blocks (design F11) and the whole budget here is smaller
 * than that. The correlation is over a window of the capture against the input
 * file, normalised so the peak is a similarity rather than an energy, and the
 * lag it reports is in FRAMES.
 *
 *   filename     the capture dump (dump-playback-capture wrote it)
 *   inputFile    the WAV that was played into the input
 *   channel      which channel of the capture (default 0)
 *   startFrame   where in the capture to take the window (default 24000)
 *   frameCount   how long a window (default 24000 = 0.5 s at 48 kHz)
 *   maxFrames    the bound the measured lag must not exceed
 *   searchFrames how far to search (default 24000)
 *   minCorrelation  the peak must beat this, or the two are not the same
 *                   signal at all and a "lag" would be noise (default 0.5)
 */
class SAssertMonitorLatencyAction : public SAction
{
public:
    SAssertMonitorLatencyAction() = default;
    QString name() const override
    { return QStringLiteral( "assert-monitor-latency" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString filename_;
    QString inputFile_;
    int     channel_      = 0;
    qint64  startFrame_   = 24000;
    qint64  frameCount_   = 24000;
    qint64  maxFrames_    = 8192;
    qint64  searchFrames_ = 24000;
    double  minCorrelation_ = 0.5;
};

/**
 * `assert-audio-continuity` - the signal never stops and never jumps.
 *
 * Two independent measurements over one pass, because a handover can fail in
 * two different ways and neither subsumes the other:
 *
 *   maxGapFrames  the longest RUN of frames below `silenceLevel`. A live lane
 *                 handing back to the frozen sum drops out for exactly this
 *                 many frames when the epoch gate is wrong.
 *   maxStep       the largest |x[n] - x[n-1]|. A discontinuity that is not a
 *                 silence - two sources summed for a block, or a block served
 *                 twice - shows up here and nowhere else.
 */
class SAssertAudioContinuityAction : public SAction
{
public:
    SAssertAudioContinuityAction() = default;
    QString name() const override
    { return QStringLiteral( "assert-audio-continuity" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString filename_;
    int     channel_       = 0;
    qint64  startFrame_    = 0;
    qint64  frameCount_    = -1;
    qint64  maxGapFrames_  = -1;     // < 0 == not checked
    double  maxStep_       = -1.0;   // < 0 == not checked
    double  silenceLevel_  = 0.001;
};

/**
 * `assert-render-policy` - nobody rendered where they were not allowed to.
 *
 * `liveThreadRefusals` counts freezes reached from the PUMP thread (which is
 * `RenderPolicy::Never`); `liveOwnedRefusals` counts freeze-path renders that
 * arrived at a live-owned processor. Both are MAXIMA and both default to 0.
 *
 * A non-zero bound is legal and is sometimes the honest answer - the disarm
 * hand-back deliberately overlaps ownership for a bounded window so the ring
 * can cover the gap (design D2's epoch gate) - but it must be spelled out in
 * the case that wants it, never defaulted.
 */
class SAssertRenderPolicyAction : public SAction
{
public:
    SAssertRenderPolicyAction() = default;
    QString name() const override
    { return QStringLiteral( "assert-render-policy" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    qint64 maxLiveThreadRefusals_ = 0;
    qint64 maxLiveOwnedRefusals_  = 0;
    // A MINIMUM, for the one case that asserts the guard FIRED (proposal 21 L2
    // AC4). "At most 0" and "at least 1" are different claims and a case that
    // wants the second must say so; defaulting to 0 keeps every existing case
    // meaning exactly what it meant.
    qint64 minLiveOwnedRefusals_  = 0;
};

/**
 * `assert-input-meter` - the PRE-FX input meter of an armed track.
 *
 * It reads the peak the live source itself recorded of what it pulled out of
 * the device ring, which is the pre-FX tap design D9 names. Deliberately not a
 * `twLevelProbe` read: there is no frozen page for a live input, and the whole
 * point of the input meter is to show signal BEFORE the chain does anything
 * to it.
 */
class SAssertInputMeterAction : public SAction
{
public:
    SAssertInputMeterAction() = default;
    QString name() const override
    { return QStringLiteral( "assert-input-meter" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    double     minPeak_ = -1.0;
    double     maxPeak_ = -1.0;
    QString    contains_;
};

/**
 * `assert-audio-onset` - WHERE a sound starts, in frames, and how long after
 * the note that asked for it (proposal 21 L2).
 *
 * It scans a capture dump forward for the first frame whose short-window RMS
 * crosses `threshold` and REPORTS that frame - the number is printed whether
 * the bounds pass or fail, because "the live instrument sounded 6 143 frames
 * after the key" is the measurement the acceptance criterion asks to be
 * recorded, not a boolean.
 *
 *   filename     the capture dump (dump-playback-capture wrote it)
 *   channel      default 0
 *   startFrame   where to start looking (default 0)
 *   threshold    the RMS a window must beat to count as sound (default 0.02,
 *                which is ~34 dB above one 16-bit lsb: well clear of dither and
 *                far below anything a case deliberately plays)
 *   window       the RMS window, in frames (default 64 - about 1.3 ms, short
 *                enough to place an onset inside one device block)
 *   minFrame / maxFrame   bounds on the onset, both optional (< 0 = unbounded)
 *
 *   afterMidiIn="1"  measure the onset RELATIVE to the last `midi-in-event` /
 *                    `midi-in-replay` injection: the injection's host time is
 *                    mapped to a capture frame through
 *                    `CaptureBackend::frameAtHostTime` - the AUDIO backend's
 *                    own block log, which shares no code with the live lane -
 *                    and `minFrame`/`maxFrame` then bound the DIFFERENCE. This
 *                    is the same independent-measurement arrangement
 *                    `assert-midi-out` uses and requires the capture audio
 *                    backend and `SMARAGD_CAPTURE_SPEED=1`.
 */
class SAssertAudioOnsetAction : public SAction
{
public:
    SAssertAudioOnsetAction() = default;
    QString name() const override
    { return QStringLiteral( "assert-audio-onset" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString filename_;
    int     channel_    = 0;
    qint64  startFrame_ = 0;
    double  threshold_  = 0.02;
    qint64  window_     = 64;
    qint64  minFrame_   = -1;
    qint64  maxFrame_   = -1;
    bool    afterMidiIn_ = false;
};

#endif // _SLIVETESTACTIONS_H_
