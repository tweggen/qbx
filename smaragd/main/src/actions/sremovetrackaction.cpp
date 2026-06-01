#include "actions/sremovetrackaction.h"
#include "sproject.h"
#include "sstdmixer.h"
#include <QDomElement>

SRemoveTrackAction::SRemoveTrackAction(int index)
    : index_(index)
{
}

SApplyResult SRemoveTrackAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        return {false, nullptr};
    }

    mixer->removeTrack(index_);

    // Phase 1: undo not implemented.
    return {true, nullptr};
}

void SRemoveTrackAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("index", index_);
}

bool SRemoveTrackAction::readXml(const QDomElement &elem, int /*version*/)
{
    index_ = elem.attribute("index", "0").toInt();
    return true;
}
