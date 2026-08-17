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
#include <QDebug>
#include <QByteArray>
#include <QDomElement>
#include <QString>
#include <cstdint>
#include <vector>

SInsertPluginAction::SInsertPluginAction(
    const QString &trackPath,
    int slotIndex,
    const audio::twPluginDescriptor &descriptor,
    const QString &stateBase64
)
    : trackPath_(trackPath),
      slotIndex_(slotIndex),
      format_(QString::fromStdString(descriptor.format)),
      uid_(QString::fromStdString(descriptor.uid)),
      pluginName_(QString::fromStdString(descriptor.name)),
      vendor_(QString::fromStdString(descriptor.vendor)),
      path_(QString::fromStdString(descriptor.path)),
      nIn_(descriptor.io.audioInputs),
      nOut_(descriptor.io.audioOutputs),
      isInstrument_(descriptor.isInstrument ? 1 : -1),
      state_(stateBase64)
{
}

// Module-path resolution lives on SPluginSlot (M4): the LOAD path needs exactly
// the same rule, and a second copy here is how the two would drift.
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
    // The path is stored AS THE CALLER GAVE IT (typically relative, e.g. the
    // in-repo "twtestclap.clap"): the slot resolves it for instantiation but
    // serializes the raw form, which is what keeps a saved project portable.
    desc.path = path_.toStdString();
    desc.io = {static_cast<std::uint16_t>(nIn_), static_cast<std::uint16_t>(nOut_)};

    // IS IT AN INSTRUMENT (design D3)? The flag decides where the slot lands and
    // whether a second one is refused, so it must survive the trip through the
    // action — reconstructing the descriptor without it made every inserted
    // instrument look like an effect and left the event feed unwired.
    //
    // An explicit attribute wins. Without one we ask the registry, which is what
    // makes `insert-plugin uid='tw.native.303'` work in a hand-written script
    // with no extra ceremony: the built-ins are always there. A module that was
    // never scanned (the in-repo CLAP/VST3 fixtures are inserted by path, not
    // found by a scan) has to say so.
    if (isInstrument_ >= 0) {
        desc.isInstrument = (isInstrument_ == 1);
    } else {
        audio::twPluginDescriptor known;
        if (audio::pluginRegistry().findByUid(desc.format, desc.uid, known))
            desc.isInstrument = known.isInstrument;
    }

    // ONE INSTRUMENT PER TRACK, ALWAYS SLOT 0 (D3). A second is REFUSED rather
    // than appended: there is one event feed per track, so a second instrument
    // would either duplicate every note or silently take none.
    if (desc.isInstrument && track->instrumentSlot()) {
        qWarning() << "insert-plugin: track" << trackPath_
                   << "already has an instrument in slot 0; refusing"
                   << QString::fromStdString(desc.uid);
        return {false, nullptr};
    }

    // Create the slot
    SPluginSlot *slot = new SPluginSlot(project, desc);
    slot->setSName(pluginName_);

    // The state chunk goes in BEFORE the link is parented. setParent() is what
    // fires slotInserted -> STrack::onPluginSlotInserted -> setChannelCount(),
    // which is the moment the plugin instances are created; restoreState() on a
    // slot with no processor yet stores the blob, and ensureChannels() replays
    // it onto every instance as they appear. That is exactly the project-load ordering,
    // so undo-of-a-removal and a file load take the same path.
    if (!state_.isEmpty()) {
        const QByteArray decoded = QByteArray::fromBase64(state_.toLatin1());
        std::vector<std::uint8_t> blob(decoded.begin(), decoded.end());
        slot->restoreState(blob);
    }

    // Add to chain
    // IMPORTANT: SLink must be constructed with parent=NULL, then setParent() called after.
    // This avoids triggering childEvent() during construction on an incompletely-initialized object.
    SLink *link = new SLink(*slot, nullptr);
    int landingIndex = chain->childCount();
    int actualIndex = (slotIndex_ < 0 || slotIndex_ > landingIndex) ? landingIndex : slotIndex_;
    if (desc.isInstrument) {
        // The instrument is slot 0 by construction, whatever the caller asked
        // for: "an instrument is a slot with a role" only works while the role
        // and the position agree (D3).
        actualIndex = 0;
    } else if (actualIndex == 0 && track->instrumentSlot()) {
        // ...and nothing may land in front of it. Same rule reorder-plugin
        // enforces; here it is a clamp rather than a refusal, because the
        // caller asked for "first effect" and that is slot 1.
        actualIndex = 1;
    }

    // Set the Qt parent (this triggers childEvent and registers in chain's childOrder_)
    link->setParent(chain);

    if (actualIndex != landingIndex) {
        // reorderSlot(), NOT moveChildToIndex(): setParent() above APPENDED the
        // tap to every twPluginChain, so moving the link in childOrder_ alone
        // would leave the DSP order as the append order. Inserting a plugin
        // anywhere but the end then desynced model and audio immediately.
        chain->reorderSlot(landingIndex, actualIndex);
    }

    // The channel-mismatch mapping (proposal 08 §Layer 3) is chosen from the
    // track's CHANNEL count — so the slot has to learn it before its insert is
    // built. The slotInserted signal above already did this for the normal
    // path; doing it again is idempotent and covers a track whose width moved.
    slot->setChannelCount(track->getChannels());

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
    elem.setAttribute("path", path_);
    elem.setAttribute("nIn", nIn_);
    elem.setAttribute("nOut", nOut_);
    // Only when TRUE: an omitted attribute means "ask the registry", which is
    // what every pre-P3b script relies on, and writing `false` would turn that
    // into an explicit claim.
    if (isInstrument_ == 1) {
        elem.setAttribute("isInstrument", "true");
    }
    // Optional: omitted (not written empty) so every pre-M5 action script and
    // every hand-written .qxa stays byte-identical when re-serialized.
    if (!state_.isEmpty()) {
        elem.setAttribute("state", state_);
    }
}

bool SInsertPluginAction::readXml(const QDomElement &elem, int /*version*/)
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
    if (elem.hasAttribute("isInstrument")) {
        const QString v = elem.attribute("isInstrument");
        isInstrument_ = (v == "true" || v == "1") ? 1 : 0;
    } else {
        isInstrument_ = -1;
    }
    state_ = elem.attribute("state");
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
