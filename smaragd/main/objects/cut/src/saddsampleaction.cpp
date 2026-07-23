#include "app/objects/cut/saddsampleaction.h"
#include "app/model/splacements.h"
#include "app/objects/cut/sremovesampleaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/cut/scut.h"
#include "app/model/slink.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

SAddSampleAction::SAddSampleAction(int trackIdx, const QString &filePath, offset_t timePos)
    : trackIndex_(trackIdx), filePath_(filePath), timePos_(timePos)
{
}

SAddSampleAction::SAddSampleAction(int trackIdx, const QString &filePath, offset_t timePos,
                                   const Fraction &srcStart, length_t cutDuration,
                                   length_t loopLength, const twGrainParams &grain)
    : trackIndex_(trackIdx), filePath_(filePath), timePos_(timePos),
      hasWindow_(true), srcStart_(srcStart), cutDuration_(cutDuration),
      loopLength_(loopLength), grain_(grain)
{
}

SApplyResult SAddSampleAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = splacements::rootContainer( project );
    if (!root || !root->isPathContainer()) {
        return {false, nullptr};
    }

    // Get the track at the specified index.
    SLink *trackLink = root->childAt(trackIndex_);
    if (!trackLink) {
        return {false, nullptr};
    }

    SObject *track = &trackLink->getSObject();
    if (!track->isPathContainer()) {
        return {false, nullptr};   // addressed child is not a lane
    }

    // Link to the file.
    QString mutablePath = filePath_;
    SLink *wavLink = project->linkToFile(mutablePath);
    if (!wavLink) {
        return {false, nullptr};
    }

    // Create a cut over the WAV. The cut makes its own content link; the
    // temporary from linkToFile() is deleted AFTER the cut exists, so the
    // wave's refcount never touches zero in between.
    SCut *cut = new SCut(project, wavLink->getSObject());
    delete wavLink;
    if (!cut) {
        return {false, nullptr};
    }

    // Restore the exact window when we were given one (undo of a deletion).
    // Same order as the clone path in SDuplicateClipAction: grain params RAW
    // first — setPitchCents()/setGrainParams() would preserve-span-rescale and
    // move the window we are about to set — then one setWindow() to publish and
    // rebuild the chain once.
    if (hasWindow_) {
        cut->setGrainParamsRaw(grain_);
        cut->setWindow(srcStart_, ClipLen(cutDuration_),
                       WarpedLen(loopLength_), grain_.stretch);
    }

    // Create a new link from the cut. IMPORTANT: construct with NULL parent,
    // then setParent after construction to avoid triggering childEvent during init.
    SLink *cutLink = new SLink(*cut, NULL);
    if (!cutLink) {
        return {false, nullptr};
    }

    // Set the start time.
    cutLink->setStartTime(timePos_);

    // Now parent the link to the track (safe: SLink is fully constructed).
    cutLink->setParent(track);

    // Find the newly created clip in the track's children to get its index.
    int clipIndex = track->indexOfChild(cutLink);
    if (clipIndex < 0) {
        return {false, nullptr};
    }

    // Return inverse: remove the sample at the same position
    SRemoveSampleAction *inverse = new SRemoveSampleAction(trackIndex_, clipIndex, filePath_, timePos_);
    return {true, inverse};
}

void SAddSampleAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackIndex", trackIndex_);
    elem.setAttribute("filePath", filePath_);
    elem.setAttribute("timePos", QString::fromStdString(Fraction(timePos_, 1).toString()));
    // Window attributes are written ONLY for the windowed form, so a plain
    // add-sample serializes exactly as it always did.
    if (hasWindow_) {
        elem.setAttribute("srcStart", QString::fromStdString(srcStart_.toString()));
        elem.setAttribute("cutDuration", QString::fromStdString(Fraction(cutDuration_, 1).toString()));
        elem.setAttribute("loopLength", QString::fromStdString(Fraction(loopLength_, 1).toString()));
        elem.setAttribute("stretch", QString::fromStdString(grain_.stretch.toString()));
        elem.setAttribute("pitchCents", grain_.pitchCents);
        elem.setAttribute("grainSize", QString::number((qulonglong)grain_.grainSize));
        elem.setAttribute("crossfade", QString::number((qulonglong)grain_.crossfade));
    }
}

bool SAddSampleAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackIndex_ = elem.attribute("trackIndex", "0").toInt();
    filePath_ = elem.attribute("filePath", "");
    // Preserve precision for large offset_t values by checking denominator
    Fraction frac = parseFractionOrDouble(elem.attribute("timePos", "0").toStdString());
    if (frac.denominator == 1) {
        timePos_ = frac.numerator;  // Exact integer conversion
    } else {
        timePos_ = (offset_t)frac.toDouble();  // Fallback for fractional times
    }

    // A cutDuration is what makes this the windowed form; without it we wrap
    // the whole wave, which is every pre-existing <add-sample/>.
    hasWindow_ = elem.hasAttribute("cutDuration");
    if (hasWindow_) {
        srcStart_ = parseFractionOrDouble(elem.attribute("srcStart", "0").toStdString());
        cutDuration_ = (length_t)parseFractionOrDouble(
            elem.attribute("cutDuration", "0").toStdString()).toDouble();
        loopLength_ = (length_t)parseFractionOrDouble(
            elem.attribute("loopLength", "0").toStdString()).toDouble();
        grain_.stretch = parseFractionOrDouble(elem.attribute("stretch", "1").toStdString());
        grain_.pitchCents = elem.attribute("pitchCents", "0").toDouble();
        grain_.grainSize = (length_t)elem.attribute("grainSize", "2048").toLongLong();
        grain_.crossfade = (length_t)elem.attribute("crossfade", "512").toLongLong();
    }
    return true;
}

static const bool s_reg_addsample = (
    SActionRegistry::instance().registerType(
        QStringLiteral("add-sample"),
        []{ return new SAddSampleAction; }
    ), true
);
