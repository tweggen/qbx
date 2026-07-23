#include "app/objects/mixer/sremoveassetplacementaction.h"
#include "app/model/splacements.h"
#include "app/objects/mixer/splaceassetaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SRemoveAssetPlacementAction::SRemoveAssetPlacementAction(const QString &assetName,
                                                         const QList<int> &trackPath,
                                                         int clipIdx,
                                                         offset_t timePos)
    : assetName_(assetName), trackPath_(trackPath), clipIdx_(clipIdx), timePos_(timePos)
{
}

SApplyResult SRemoveAssetPlacementAction::apply(SProject *project)
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

    // Get the placed asset link at the specified index.
    SLink *assetLink = track->childAt(clipIdx_);
    if (!assetLink) {
        return {false, nullptr};
    }

    // Delete the link (this drops the refcount; the registry's extra ref keeps the asset alive).
    delete assetLink;

    // Return inverse: re-place the asset at the same position
    SPlaceAssetAction *inverse = new SPlaceAssetAction(assetName_, trackPath_, timePos_);
    return {true, inverse};
}

// No longer live-only: the timeline routes deletion of an asset PLACEMENT here
// (the clip is the registered body, so undo must re-place it rather than
// rebuild a lookalike), and that path needs to be drivable from a test script.
void SRemoveAssetPlacementAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("assetName", assetName_);
    elem.setAttribute("trackPath", pathToString(trackPath_));
    elem.setAttribute("clipIdx", clipIdx_);
    elem.setAttribute("timePos", QString::fromStdString(Fraction(timePos_, 1).toString()));
}

bool SRemoveAssetPlacementAction::readXml(const QDomElement &elem, int /*version*/)
{
    assetName_ = elem.attribute("assetName", "");
    trackPath_ = stringToPath(elem.attribute("trackPath"));
    clipIdx_ = elem.attribute("clipIdx", "0").toInt();
    Fraction frac = parseFractionOrDouble(elem.attribute("timePos", "0").toStdString());
    timePos_ = ( frac.denominator == 1 ) ? (offset_t) frac.numerator
                                         : (offset_t) frac.toDouble();
    return true;
}

static const bool s_reg_removeassetplacement = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-asset-placement"),
        []{ return new SRemoveAssetPlacementAction; }
    ), true
);
