#ifndef SASSERTCLIPCHANNELSACTION_H
#define SASSERTCLIPCHANNELSACTION_H

#include "app/actions/saction.h"
#include "app/model/sobjectpath.h"

/**
 * Assertion action: the component a CLIP resolves to publishes a page of the
 * expected width, with genuinely different channels — proposal 36 AC B3.2.
 *
 * WHY THIS IS NOT assert-channels-differ. That verb reads a rendered WAV, and
 * when B3 wrote this one the sink still collapsed the graph to one bus and
 * duplicated it, so no file on disk could show a clip's second channel. B5 has
 * since widened the sink, so a file CAN show one — but it shows the audio after
 * the track mix, the plugin chain, the rewire and the master sum. This verb
 * still reads the page the RESOLVED CLIP COMPONENT publishes, which is the only
 * place a clip's own width is observable without everything downstream in the
 * way, and it is what makes a failure name the clip path rather than the sink.
 *
 * WHY NOT THE TRACK'S ROOT COMPONENT. Because it did not exist at width 2 when
 * B3 wrote this: twTrackMix and twPluginChain allocated their own width-1 pages
 * until B4. The track-root assertion is AC B4.7, and it is
 * assert-track-channels.
 *
 * THROUGH THE REAL SCHEDULER, not a hand-rolled freeze. It declares a demand on
 * the project's CaptureRevalidator (requestGraphPages) and waits, which is the
 * same path playback readahead and the offline render take: the page is planned,
 * dependency-counted and executed on a worker, and lands in the component's own
 * cache. assert-meter's tap->requestPage() is the LEGACY PULL by comparison, and
 * it carries a caveat about content epochs that this verb does not want.
 *
 * XML format:
 * <assert-clip-channels clip="0,0" position="0" expectChannels="2"
 *                       channelA="0" channelB="1"
 *                       minRmsDelta="0.15" minDiffRms="0.15"/>
 *
 * Parameters:
 * - clip:            path to the clip's SLink (same spelling slip-clip uses)
 * - position:        CLIP-RELATIVE frame position to resolve and probe
 * - expectChannels:  required page width (0 = do not check)
 * - channelA/B:      the two channels compared; both must exist on the page
 * - minRmsDelta:     minimum |rms(A) - rms(B)| (a LEVEL discriminator)
 * - minDiffRms:      minimum rms(A - B); < 0 = not checked (a CONTENT one)
 *
 * `expectReject="true"` asserts the opposite, which is how a MONO clip is
 * pinned as still-mono without a second verb.
 */
class SAssertClipChannelsAction : public SAction {
public:
    SAssertClipChannelsAction() = default;

    QString name() const override { return QStringLiteral("assert-clip-channels"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("clip"), QStringLiteral("position"),
                QStringLiteral("expectChannels"), QStringLiteral("channelA"),
                QStringLiteral("channelB"), QStringLiteral("minRmsDelta"),
                QStringLiteral("minDiffRms")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> clipPath_;
    int64_t position_ = 0;
    int     expectChannels_ = 0;
    int     channelA_ = 0;
    int     channelB_ = 1;
    double  minRmsDelta_ = 0.01;
    double  minDiffRms_ = -1.0;
};

#endif // SASSERTCLIPCHANNELSACTION_H
