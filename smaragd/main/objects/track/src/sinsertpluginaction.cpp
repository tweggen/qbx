#include "app/objects/track/sinsertpluginaction.h"
#include "app/model/splacements.h"
#include "app/objects/track/sremovepluginaction.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strackpath.h"
#include "tw/plugins/twplugindescriptor.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>
#include <QString>

SInsertPluginAction::SInsertPluginAction(
    const QString &trackPath,
    int slotIndex,
    const audio::twPluginDescriptor &descriptor
)
    : trackPath_(trackPath),
      slotIndex_(slotIndex),
      format_(QString::fromStdString(descriptor.format)),
      uid_(QString::fromStdString(descriptor.uid)),
      pluginName_(QString::fromStdString(descriptor.name)),
      vendor_(QString::fromStdString(descriptor.vendor)),
      nIn_(descriptor.io.audioInputs),
      nOut_(descriptor.io.audioOutputs)
{
}

SApplyResult SInsertPluginAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    // Parse the track path (format: "/mixer/N" where N is track index)
    auto path = strackpath::stringToPath(trackPath_);
    SObject *root = project->getRootComponent();
    SObject *trackObj = strackpath::resolveByPath(root, path);
    STrack *track = dynamic_cast<STrack*>(trackObj);
    if (!track) {
        return {false, nullptr};
    }

    // Get the plugin chain
    SPluginChain *chain = track->getPluginChain();
    if (!chain) {
        return {false, nullptr};
    }

    // Reconstruct the descriptor
    audio::twPluginDescriptor desc;
    desc.format = format_.toStdString();
    desc.uid = uid_.toStdString();
    desc.name = pluginName_.toStdString();
    desc.vendor = vendor_.toStdString();
    desc.io = {nIn_, nOut_};

    // Create the slot
    SPluginSlot *slot = new SPluginSlot(project, desc);
    slot->setSName(pluginName_);

    // Add to chain
    // IMPORTANT: SLink must be constructed with parent=NULL, then setParent() called after.
    // This avoids triggering childEvent() during construction on an incompletely-initialized object.
    SLink *link = new SLink(*slot, nullptr);
    int landingIndex = chain->childCount();
    int actualIndex = (slotIndex_ < 0 || slotIndex_ > landingIndex) ? landingIndex : slotIndex_;

    // Set the Qt parent (this triggers childEvent and registers in chain's childOrder_)
    link->setParent(chain);

    if (actualIndex != landingIndex) {
        chain->moveChildToIndex(landingIndex, actualIndex);
    }

    // Create inverse action
    SAction *inverse = new SRemovePluginAction(trackPath_, actualIndex);

    return {true, inverse};
}

void SInsertPluginAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackPath", trackPath_);
    elem.setAttribute("slotIndex", slotIndex_);
    elem.setAttribute("format", format_);
    elem.setAttribute("uid", uid_);
    elem.setAttribute("name", pluginName_);
    elem.setAttribute("vendor", vendor_);
    elem.setAttribute("nIn", nIn_);
    elem.setAttribute("nOut", nOut_);
}

bool SInsertPluginAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackPath_ = elem.attribute("trackPath");
    slotIndex_ = elem.attribute("slotIndex", "0").toInt();
    format_ = elem.attribute("format");
    uid_ = elem.attribute("uid");
    pluginName_ = elem.attribute("name");
    vendor_ = elem.attribute("vendor");
    nIn_ = elem.attribute("nIn", "0").toUInt();
    nOut_ = elem.attribute("nOut", "0").toUInt();
    return true;
}

// Register the action type
namespace {
    const bool _reg = (
        SActionRegistry::instance().registerType(
            QStringLiteral("insert-plugin"),
            []{ return new SInsertPluginAction("", -1,
                audio::twPluginDescriptor{}); }
        ), true
    );
}
