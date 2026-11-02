#ifndef SASSERTAUDIOPERIODACTION_H
#define SASSERTAUDIOPERIODACTION_H

#include "app/actions/saction.h"

/**
 * assert-audio-period — the PERIOD of a rendered region, in milliseconds
 * (proposal 56 Q4.1, QBX-268: "a 2-beat loop at 120 BPM played at 100 BPM by
 * stretch measures a period of 2*60/100 s +- 1 ms"; nassau-digital GATES C7.1
 * measures the core's own the same way).
 *
 * Normalised autocorrelation (audio::estimatePeriodFrames), the global
 * maximum over lags [minMs, maxMs], refined by a parabola. The search range
 * bounds the cost (the period of a loop is long) and is chosen AROUND the
 * expected period but wider than the tolerance, so a wrong period shows as a
 * number off by more than the tolerance — a maximum on the range's edge FAILS
 * (the period is not inside the range at all). The measured value is REPORTED
 * whether the check passes or fails.
 *
 * XML:
 * <assert-audio-period filename="render.wav" channel="0"
 *                      startFrame="48000" frameCount="288000"
 *                      minMs="1150" maxMs="1250"
 *                      expectMs="1200" toleranceMs="1"/>
 *
 * filename (REQUIRED; relative to the test output dir), channel = "0" (-1 =
 * all channels mixed), startFrame = "0", frameCount = "-1", minMs, maxMs (the
 * lag range, REQUIRED), expectMs and toleranceMs (REQUIRED: the check).
 */
class SAssertAudioPeriodAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-audio-period" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString filename_;
    int     channel_     = 0;
    qint64  startFrame_  = 0;
    qint64  frameCount_  = -1;
    double  minMs_       = 0.0;
    double  maxMs_       = 0.0;
    double  expectMs_    = 0.0;
    double  toleranceMs_ = -1.0;
};

#endif
