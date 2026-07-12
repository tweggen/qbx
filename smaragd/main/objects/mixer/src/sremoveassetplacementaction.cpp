#include "app/objects/mixer/sremoveassetplacementaction.h"
#include "app/objects/mixer/splaceassetaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
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

    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        return {false, nullptr};
    }

    // Resolve the destination track via path from the root mixer.
    SObject *trackObj = resolveByPath(root, trackPath_);
    if (!trackObj) {
        return {false, nullptr};
    }

    STrack *track = dynamic_cast<STrack*>(trackObj);
    if (!track) {
        return {false, nullptr};
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

void SRemoveAssetPlacementAction::writeXml(QDomElement &/*elem*/) const
{
    // Live-only action — never serialized. Intentionally empty.
}

bool SRemoveAssetPlacementAction::readXml(const QDomElement &/*elem*/, int /*version*/)
{
    // Live-only action — never deserialized.
    return false;
}
