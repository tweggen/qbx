#include "app/objects/mixer/sremovepluginaction.h"
#include "app/objects/mixer/sinsertpluginaction.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"
#include "app/objects/mixer/spluginchain.h"
#include "app/pluginui/spluginslot.h"
#include "app/model/slink.h"
#include "app/objects/track/strackpath.h"
#include "tw/plugins/twplugindescriptor.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>
#include <QString>

SRemovePluginAction::SRemovePluginAction(const QString &trackPath, int slotIndex)
    : trackPath_(trackPath), slotIndex_(slotIndex),
      nIn_(0), nOut_(0)
{
}

SApplyResult SRemovePluginAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    // Parse the track path
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

    // Get the slot to remove
    SPluginSlot *slot = chain->getSlotAt(slotIndex_);
    if (!slot) {
        return {false, nullptr};
    }

    // Save the descriptor for the inverse action
    const auto &desc = slot->getDescriptor();
    format_ = QString::fromStdString(desc.format);
    uid_ = QString::fromStdString(desc.uid);
    pluginName_ = QString::fromStdString(desc.name);
    vendor_ = QString::fromStdString(desc.vendor);
    nIn_ = desc.io.audioInputs;
    nOut_ = desc.io.audioOutputs;

    // Remove the slot (SLink will be deleted, which deletes the SPluginSlot)
    SLink *link = chain->childAt(slotIndex_);
    if (link) {
        delete link;
    }

    // Create inverse action
    audio::twPluginDescriptor desc_inv;
    desc_inv.format = format_.toStdString();
    desc_inv.uid = uid_.toStdString();
    desc_inv.name = pluginName_.toStdString();
    desc_inv.vendor = vendor_.toStdString();
    desc_inv.io = {nIn_, nOut_};

    SAction *inverse = new SInsertPluginAction(trackPath_, slotIndex_, desc_inv);

    return {true, inverse};
}

void SRemovePluginAction::writeXml(QDomElement &elem) const
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

bool SRemovePluginAction::readXml(const QDomElement &elem, int /*version*/)
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
            QStringLiteral("remove-plugin"),
            []{ return new SRemovePluginAction("", -1); }
        ), true
    );
}
