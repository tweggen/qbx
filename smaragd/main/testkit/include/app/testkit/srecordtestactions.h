#ifndef _SRECORDTESTACTIONS_H_
#define _SRECORDTESTACTIONS_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"

/**
 * The AUDIO RECORDING verbs (proposal 21 L3b).
 *
 * `record-start` / `record-stop` are TRANSPORT-level and ABSOLUTE — start
 * begins a take on every armed track, stop ends it and commits the placement
 * as one undo step. They are test-only in the sense every transport verb is:
 * not undoable, and they drive the same `SApplication` entry points the record
 * button does, so a script exercises the production path rather than a copy of
 * it.
 *
 *   <record-start/>
 *   <record-stop/>
 *
 * `assert-recorded-clip` is the placement gate.
 *
 *   <assert-recorded-clip trackPath="0"
 *                         clips="1" takes="-1"
 *                         startFrame="-1" startTolerance="1024"
 *                         durationFrames="-1" durationTolerance="4096"
 *                         inputLatencyFrames="-1" outputLatencyFrames="-1"
 *                         userOffsetFrames="-999999"
 *                         compensationFrames="-999999"
 *                         trimmedFrames="-1" passes="-1"
 *                         growing="false" previewNonEmpty="false"
 *                         sourceAtStartFrame="-1" sourceTolerance="4096"
 *                         minDurationFrames="-1"/>
 *
 * Every numeric expectation is opt-in (the sentinel means "unchecked"), so one
 * verb serves the mid-take assertions and the post-take ones.
 *
 * WHAT IS AND IS NOT A CLOSED FORM HERE, because it decides what the gates can
 * claim. `P0` — the project frame capture frame 0 maps to — depends on when the
 * capture thread and the render callback actually ran, so it is NOT
 * predictable and no case asserts it. What IS exact, in integers, is the
 * COMPENSATION: `startFrame - P0 - trimmed`, which the conversion defines as
 * `-inputLatency - outputLatency + userOffset`. A case that sets the reported
 * input latency (`SMARAGD_AUDIO_INPUT_LATENCY_FRAMES`) and knows the capture
 * backend's output latency can predict that number to the frame, and the verb
 * ALSO checks the identity internally — so a recorder whose stored terms and
 * actual placement disagree fails even when the expected value is not given.
 *
 * `sourceAtStartFrame` is the physical claim, and it is about FAITHFULNESS
 * rather than about latency: it decodes the position-encoded fixture at the
 * placed clip's first content frame and compares it to the number of capture
 * frames the mapping says were trimmed. They agree only if every frame the
 * file produced reached the pages in order, with none lost or duplicated
 * between the device ring, the bridge, the growing source and the placement.
 */
class SRecordStartAction : public SAction {
public:
    SRecordStartAction() = default;
    QString name() const override { return QStringLiteral( "record-start" ); }
    QStringList knownAttributes() const override { return {}; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
};

class SRecordStopAction : public SAction {
public:
    SRecordStopAction() = default;
    QString name() const override { return QStringLiteral( "record-stop" ); }
    QStringList knownAttributes() const override { return {}; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
};

class SAssertRecordedClipAction : public SAction {
public:
    SAssertRecordedClipAction() = default;
    QString name() const override
    { return QStringLiteral( "assert-recorded-clip" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    static constexpr qint64 kUnset  = -1;
    static constexpr qint64 kUnsetS = -999999;   // for signed expectations

    QList<int> trackPath_;
    qint64 clips_             = kUnset;
    qint64 takes_             = kUnset;
    qint64 startFrame_        = kUnset;
    qint64 startTolerance_    = 1024;
    qint64 durationFrames_    = kUnset;
    qint64 durationTolerance_ = 4096;
    qint64 minDurationFrames_ = kUnset;
    qint64 inputLatency_      = kUnset;
    qint64 outputLatency_     = kUnset;
    qint64 userOffset_        = kUnsetS;
    qint64 compensation_      = kUnsetS;
    qint64 trimmed_           = kUnset;
    qint64 passes_            = kUnset;
    bool   growing_           = false;
    bool   previewNonEmpty_   = false;
    qint64 sourceAtStart_     = kUnset;
    qint64 sourceTolerance_   = 4096;
};

#endif // _SRECORDTESTACTIONS_H_
