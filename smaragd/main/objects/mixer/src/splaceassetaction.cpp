#include "app/objects/mixer/splaceassetaction.h"
#include "app/model/splacements.h"
#include "app/objects/mixer/sremoveassetplacementaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/cut/scut.h"
#include "app/model/slink.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SPlaceAssetAction::SPlaceAssetAction(const QString &assetName,
                                     const QList<int> &trackPath,
                                     offset_t timePos)
    : assetName_(assetName), trackPath_(trackPath), timePos_(timePos)
{
}

SApplyResult SPlaceAssetAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = splacements::rootContainer( project );
    SObject *mixer = root;
    if (!mixer) {
        return {false, nullptr};
    }

    // Resolve the destination track via path from the root mixer.
    SObject *trackObj = resolveByPath(root, trackPath_);
    if (!trackObj) {
        return {false, nullptr};
    }

    SObject *track = trackObj;
    if (!track->isPathContainer()) {
        return {false, nullptr};   // destination is not a lane
    }

    // Resolve the asset by name from the project registry.
    SObject *assetBody = project->asset(assetName_);
    if (!assetBody) {
        return {false, nullptr};
    }

    // Cycle guard (proposal 05 §2.7): an asset is a live window over a container.
    // Placing it inside that same container — or a descendant of it — would make
    // the container's render pull itself (the capture build recurses before the
    // snapshot exists). Refuse such placements.
    if (SCut *assetCut = dynamic_cast<SCut*>(assetBody)) {
        SObject *container = &assetCut->getContent();
        if (container == root) {
            return {false, nullptr};   // whole-mixer asset: every placement cycles
        }
        if (STrack *containerTrack = dynamic_cast<STrack*>(container)) {
            if (isSelfOrDescendant(track, containerTrack)) {
                return {false, nullptr};   // dropping into the asset's own subtree
            }
        }
    }

    // Pin the asset to keep it alive during undo/redo cycles.
    assetBody->addRef();

    // Create a new link to the asset. IMPORTANT: construct with NULL parent,
    // then setParent after construction to avoid triggering childEvent during init.
    SLink *assetLink = new SLink(*assetBody, NULL);
    if (!assetLink) {
        assetBody->removeRef();
        return {false, nullptr};
    }

    // Set the start time.
    assetLink->setStartTime(timePos_);

    // Now parent the link to the track (safe: SLink is fully constructed).
    assetLink->setParent(track);

    // Find the newly created asset placement in the track's children to get its index.
    int clipIdx = track->indexOfChild(assetLink);
    if (clipIdx < 0) {
        assetBody->removeRef();
        return {false, nullptr};
    }

    // Return inverse: remove the asset placement at the same position
    SRemoveAssetPlacementAction *inverse = new SRemoveAssetPlacementAction(
        assetName_, trackPath_, clipIdx, timePos_);
    return {true, inverse};
}

void SPlaceAssetAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("assetName", assetName_);
    elem.setAttribute("trackPath", pathToString(trackPath_));
    elem.setAttribute("timePos", QString::fromStdString(Fraction(timePos_, 1).toString()));
}

bool SPlaceAssetAction::readXml(const QDomElement &elem, int /*version*/)
{
    assetName_ = elem.attribute("assetName", "");
    trackPath_ = stringToPath(elem.attribute("trackPath", ""));
    // Preserve precision for large offset_t values by checking denominator
    Fraction frac = parseFractionOrDouble(elem.attribute("timePos", "0").toStdString());
    if (frac.denominator == 1) {
        timePos_ = frac.numerator;  // Exact integer conversion
    } else {
        timePos_ = (offset_t)frac.toDouble();  // Fallback for fractional times
    }
    return true;
}

static const bool s_reg_placeasset = (
    SActionRegistry::instance().registerType(
        QStringLiteral("place-asset"),
        []{ return new SPlaceAssetAction; }
    ), true
);
