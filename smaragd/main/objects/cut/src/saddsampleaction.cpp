#include "app/objects/cut/saddsampleaction.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/cut/sremovesampleaction.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/cut/scut.h"
#include "app/model/slink.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

SAddSampleAction::SAddSampleAction(const QList<int> &trackPath, const QString &filePath,
                                   offset_t timePos)
    : trackPath_(trackPath), filePath_(filePath), timePos_(timePos)
{
}

SAddSampleAction::SAddSampleAction(const QList<int> &trackPath, const QString &filePath,
                                   offset_t timePos,
                                   const Fraction &srcStart, length_t cutDuration,
                                   length_t loopLength, const twGrainParams &grain)
    : trackPath_(trackPath), filePath_(filePath), timePos_(timePos),
      hasWindow_(true), srcStart_(srcStart), cutDuration_(cutDuration),
      loopLength_(loopLength), grain_(grain)
{
}

SApplyResult SAddSampleAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    // The addressed root (proposal 09 D21): whatever qualifier the trackPath
    // carried, else the `arrangement` attribute, else the master.
    SObject *root = splacements::rootNamed( project, arrangement_ );
    if (!root || !root->isPathContainer()) {
        return {false, nullptr};
    }

    // Get the lane. Path-addressed, so a track nested inside a folder track
    // resolves as readily as a top-level one.
    SObject *track = splacements::laneAt( root, trackPath_ );
    if (!track) {
        return {false, nullptr};
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
    SRemoveSampleAction *inverse = new SRemoveSampleAction(trackPath_, clipIndex, filePath_, timePos_);
    return {true, inverse};
}

void SAddSampleAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackPath", strackpath::pathToString(trackPath_));
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
    // Sniff the spelling rather than key off formatVersion(): pre-existing .qxa
    // scripts carry no version attribute, and `trackIndex` is exactly a
    // one-element path.
    if (elem.hasAttribute("trackPath")) {
        // A qualified spelling ("Drums:0,1") carries its own root; a bare
        // "0,1" still means the master, which is what keeps every existing
        // case unchanged.
        const strackpath::QualifiedPath q =
            strackpath::parseQualified( elem.attribute("trackPath") );
        trackPath_   = q.idx;
        arrangement_ = q.root;
    } else {
        trackPath_ = QList<int>{ elem.attribute("trackIndex", "0").toInt() };
    }
    // The index spelling has no place to put a qualifier, so it takes one as
    // its own attribute.
    if (arrangement_.isEmpty())
        arrangement_ = elem.attribute("arrangement");
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
