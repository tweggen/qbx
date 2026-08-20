#include "app/objects/track/sremovepluginaction.h"
#include "app/model/splacements.h"
#include "app/objects/track/sinsertpluginaction.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/model/slink.h"
#include "app/objects/track/strackpath.h"
#include "tw/plugins/twplugindescriptor.h"
#include "app/actions/sactionregistry.h"
#include <QByteArray>
#include <QDomElement>
#include <QString>
#include <cstdint>
#include <vector>

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
    auto path = strackpath::parseInto( pathRoot_, trackPath_ );
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

    // Save the descriptor for the inverse action. The EFFECTIVE descriptor is
    // the one to keep: it carries the module path (and the plugin's real channel
    // counts) that the registry resolved, which is what the inverse needs to
    // load the plugin again.
    const auto &desc = slot->getEffectiveDescriptor();
    format_ = QString::fromStdString(desc.format);
    uid_ = QString::fromStdString(desc.uid);
    pluginName_ = QString::fromStdString(desc.name);
    vendor_ = QString::fromStdString(desc.vendor);
    path_ = QString::fromStdString(desc.path);
    nIn_ = desc.io.audioInputs;
    nOut_ = desc.io.audioOutputs;

    // Capture the plugin STATE too, or the inverse re-inserts a slot at plugin
    // defaults and the undo is only half an undo. saveState() pulls a fresh blob
    // from the live plugin when the slot is Active and hands back the stored blob
    // verbatim otherwise — which is what keeps a Missing plugin's patch intact
    // across remove/undo on a machine that does not have the plugin installed.
    {
        std::vector<std::uint8_t> blob;
        slot->saveState(blob);
        if (!blob.empty()) {
            const QByteArray raw((const char *) blob.data(), (int) blob.size());
            state_ = QString::fromLatin1(raw.toBase64());
        } else {
            state_.clear();
        }
    }

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
    desc_inv.path = path_.toStdString();
    desc_inv.io = {nIn_, nOut_};

    SAction *inverse = new SInsertPluginAction(trackPath_, slotIndex_, desc_inv,
                                               state_);

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
    elem.setAttribute("path", path_);
    elem.setAttribute("nIn", nIn_);
    elem.setAttribute("nOut", nOut_);
    // Optional, and only ever non-empty on an action that has already been
    // applied — a hand-written <remove-plugin> in a .qxa carries none.
    if (!state_.isEmpty()) {
        elem.setAttribute("state", state_);
    }
}

bool SRemovePluginAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackPath_ = elem.attribute("trackPath");
    slotIndex_ = elem.attribute("slotIndex", "0").toInt();
    format_ = elem.attribute("format");
    uid_ = elem.attribute("uid");
    pluginName_ = elem.attribute("name");
    vendor_ = elem.attribute("vendor");
    path_ = elem.attribute("path");
    nIn_ = elem.attribute("nIn", "0").toUInt();
    nOut_ = elem.attribute("nOut", "0").toUInt();
    state_ = elem.attribute("state");
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
