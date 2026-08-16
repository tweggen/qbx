#ifndef SASSERTTRACKCHANNELSACTION_H
#define SASSERTTRACKCHANNELSACTION_H

#include "app/actions/saction.h"
#include "app/model/sobjectpath.h"

/**
 * Assertion action: a TRACK'S ROOT COMPONENT publishes a page of the expected
 * width, with genuinely different channels — proposal 36 AC B4.7.
 *
 * WHY THIS EXISTS ALONGSIDE assert-clip-channels. B3 widened the CLIP path, and
 * that verb proved a stereo clip's width survives twView::resolve. It could not
 * be asked at the track's root, because twTrackMix and twPluginChain were still
 * width 1 there and IOVector — the seam the mix goes through — had no channel
 * selector. B4 widens all three, so the question "did the clip's second channel
 * survive the TRACK" finally has a place to be asked. It is a different claim
 * from B3's: everything between the clip and here (the mix's per-channel
 * IOVector loop, the plugin chain, the rewire's channel map) is B4's code.
 *
 * The tap is STrack::getRootComponent() — its twRewire — which is the same
 * component the meters read and the mixer sums, so an assertion here is about
 * what is actually heard rather than about an internal stage.
 *
 * TWO DRIVERS, and the choice matters (AC B4.6):
 *
 *   driver="scheduler" (default) declares a demand on the project's
 *     CaptureRevalidator and waits — the path playback readahead and the
 *     offline render take.
 *   driver="pull" calls requestPage() on the tap directly, which is the LEGACY
 *     PULL: the recursive freeze through twStreamingLatch::copyData, with no
 *     plan, no dependency counting and no bound pages. It is the path
 *     assert-meter drives, and no other AC covers it wide. It is also the only
 *     driver available under SMARAGD_REVAL_WORKERS=0, where there is no
 *     revalidator at all — so a case run that way must ask for it.
 *
 * XML format:
 * <assert-track-channels trackPath="0" position="0" expectChannels="2"
 *                        channelA="0" channelB="1"
 *                        minRmsDelta="0.15" minDiffRms="0.15"
 *                        driver="scheduler"/>
 *
 * Parameters:
 * - trackPath:       path to the track (strackpath spelling, e.g. "0" or "0,1")
 * - position:        absolute TIMELINE frame position to probe
 * - expectChannels:  required page width (0 = do not check)
 * - channelA/B:      the two channels compared; both must exist on the page
 * - minRmsDelta:     minimum |rms(A) - rms(B)| (a LEVEL discriminator)
 * - minDiffRms:      minimum rms(A - B); < 0 = not checked (a CONTENT one)
 * - minRms:          lower bound on rms(channelA); < 0 = not checked. A page of
 *                    the right width full of SILENCE would otherwise satisfy
 *                    every other bound trivially.
 * - driver:          "scheduler" (default) or "pull"
 *
 * `expectReject="true"` asserts the opposite, which is how a mono track is
 * pinned as still-mono without a second verb.
 */
class SAssertTrackChannelsAction : public SAction {
public:
    SAssertTrackChannelsAction() = default;

    QString name() const override { return QStringLiteral("assert-track-channels"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("trackPath"), QStringLiteral("position"),
                QStringLiteral("expectChannels"), QStringLiteral("channelA"),
                QStringLiteral("channelB"), QStringLiteral("minRmsDelta"),
                QStringLiteral("minDiffRms"), QStringLiteral("minRms"),
                QStringLiteral("driver")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QList<int> trackPath_;
    int64_t position_ = 0;
    int     expectChannels_ = 0;
    int     channelA_ = 0;
    int     channelB_ = 1;
    double  minRmsDelta_ = 0.01;
    double  minDiffRms_ = -1.0;
    double  minRms_ = -1.0;
    QString driver_ = QStringLiteral("scheduler");
};

/**
 * Assertion action: THE MASTER'S PAGE IS THE SUM OF THE TRACKS' PAGES, CHANNEL
 * BY CHANNEL — proposal 36 AC B4.1.
 *
 * It freezes the root mixer's component and every top-level track's root
 * component at the same page, and compares master[c][i] against the sum over
 * tracks of track[c][i], for EVERY channel of the master page and a dense
 * sample of frames. The point is not that the master is loud; it is that
 * channel k of the master is channel k of the tracks and not, say, channel 0 of
 * all of them (which is what the pre-B4 master did — SStdMixer ran `bus < 1`,
 * so every track's second channel was built, filtered, plugin-processed and
 * dropped).
 *
 * MUTED AND SOLOED-OUT TRACKS ARE EXCLUDED, because the mixer excludes them by
 * nulling their input plug — including them would make the sum wrong for a
 * reason that has nothing to do with channels.
 *
 * XML format:
 * <assert-master-sums position="0" tolerance="0.0005" stride="97"/>
 *
 * Parameters:
 * - position:   absolute TIMELINE frame position; the containing page is used
 * - tolerance:  maximum absolute per-sample deviation (float32 summation is not
 *               associative, so an exact comparison would be wrong, not strict)
 * - stride:     compare every Nth frame (default 97 — coprime with the page
 *               size, so the sampled set is not a periodic pattern)
 * - minRms:     lower bound on the master's rms on channel 0, so a project that
 *               rendered silence cannot pass the sum trivially (0 = unchecked)
 */
class SAssertMasterSumsAction : public SAction {
public:
    SAssertMasterSumsAction() = default;

    QString name() const override { return QStringLiteral("assert-master-sums"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("position"), QStringLiteral("tolerance"),
                QStringLiteral("stride"), QStringLiteral("minRms")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int64_t position_ = 0;
    double  tolerance_ = 5e-4;
    int     stride_ = 97;
    double  minRms_ = 0.0;
};

#endif // SASSERTTRACKCHANNELSACTION_H
