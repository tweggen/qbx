#ifndef SASSERTCLIPCHANNELSACTION_H
#define SASSERTCLIPCHANNELSACTION_H

#include "app/actions/saction.h"
#include "app/model/sobjectpath.h"

/**
 * Assertion action: the component a CLIP resolves to publishes a page of the
 * expected width, with genuinely different channels — proposal 36 AC B3.2.
 *
 * WHY THIS IS NOT assert-channels-differ. That verb reads a rendered WAV, and
 * the sink still collapses the graph to one bus and duplicates it (B5 fixes
 * that), so no file on disk can show a clip's second channel before B5. B3
 * widens the CLIP PATH, and the only place its result is observable is the page
 * the resolved component publishes. This verb reads exactly that.
 *
 * WHY NOT THE TRACK'S ROOT COMPONENT. Because it does not exist yet at width 2:
 * twTrackMix and twPluginChain allocate their own width-1 pages until B4, and
 * IOVector — the seam twTrackMix mixes through — is mono by design (§4.6). The
 * track-root assertion is AC B4.7.
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
