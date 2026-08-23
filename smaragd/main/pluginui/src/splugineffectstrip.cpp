#include "app/pluginui/splugineffectstrip.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/pluginui/spluginbrowserdialog.h"
#include "app/pluginui/spluginparamereditor.h"
#include "app/pluginui/spluginnativeeditor.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/model/slink.h"
#include "app/objects/track/sinsertpluginaction.h"
#include "app/objects/track/sremovepluginaction.h"
#include "app/objects/track/sreorderpluginaction.h"
#include "app/objects/track/ssetpluginbypassaction.h"
#include "tw/core/twlog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QDialog>
#include <QEvent>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QCursor>
#include <QTimer>

namespace {

const char *stateName( audio::twPluginSlotState s )
{
    switch( s ) {
        case audio::twPluginSlotState::Active:      return "Active";
        case audio::twPluginSlotState::Missing:     return "Missing";
        case audio::twPluginSlotState::Unsupported: return "Unsupported";
    }
    return "Active";
}

const char *modeName( audio::twPluginSlotMode m )
{
    switch( m ) {
        case audio::twPluginSlotMode::Transparent: return "Transparent";
        case audio::twPluginSlotMode::Direct:      return "Direct";
        case audio::twPluginSlotMode::DualMono:    return "DualMono";
        case audio::twPluginSlotMode::MonoFold:    return "MonoFold";
        // The generator rows (proposal 37 P3b, design 4.3).
        case audio::twPluginSlotMode::DirectGen:   return "DirectGen";
        case audio::twPluginSlotMode::MonoSpread:  return "MonoSpread";
        case audio::twPluginSlotMode::GenFold:     return "GenFold";
        case audio::twPluginSlotMode::WideGen:     return "WideGen";
    }
    return "Transparent";
}

// The label a row shows. ALWAYS the descriptor the PROJECT FILE carried
// (getDescriptor()), never the resolved one: for a Missing plugin the resolved
// descriptor is the placeholder's, and the whole point of the greyed row is to
// tell the user WHICH plugin is missing (CONTRACT invariant 4).
QString storedName( SPluginSlot *slot )
{
    QString name = QString::fromStdString( slot->getDescriptor().name );
    if( name.isEmpty() ) name = slot->getSName();
    if( name.isEmpty() ) name = QString::fromStdString( slot->getDescriptor().uid );
    return name;
}

QString reasonTooltip( SPluginSlot *slot )
{
    const audio::twPluginDescriptor &d = slot->getDescriptor();
    const QString ident = QStringLiteral( "%1 / %2" )
                              .arg( QString::fromStdString( d.format ),
                                    QString::fromStdString( d.uid ) );
    switch( slot->getSlotState() ) {
        case audio::twPluginSlotState::Missing:
            return SPluginEffectStrip::tr(
                       "Plugin not installed: %1\n"
                       "Vendor: %2\nStored module path: %3\n"
                       "The slot is transparent and its saved settings are kept "
                       "unchanged. Install the plugin, rescan, then press Reload." )
                .arg( ident, QString::fromStdString( d.vendor ),
                      QString::fromStdString( d.path ) );
        case audio::twPluginSlotState::Unsupported:
            return SPluginEffectStrip::tr(
                       "Unsupported channel layout: %1 declares %2 in / %3 out, "
                       "which has no defined mapping onto this track's buses.\n"
                       "The slot is transparent and its saved settings are kept "
                       "unchanged." )
                .arg( ident )
                .arg( d.io.audioInputs )
                .arg( d.io.audioOutputs );
        case audio::twPluginSlotState::Active:
            break;
    }
    return SPluginEffectStrip::tr( "%1\nVendor: %2\nChannel mapping: %3" )
        .arg( ident, QString::fromStdString( d.vendor ),
              QString::fromLatin1( modeName( slot->getSlotMode() ) ) );
}

// The generic parameter editor's window registry (fixed 2026-08-23, mirroring
// SPluginNativeEditor::registry() in spluginnativeeditor.cpp). MODULE-LEVEL,
// deliberately NOT the `editors_` member SPluginEffectStrip used to own: a
// QDialog parented under a strip is a Qt CHILD of it, so
// STrackDetailPanel::rebuildUI()'s `delete pluginStrip_` on every track
// selection change destroyed this editor exactly as it destroyed the native
// one (see main/pluginui/CONTRACT.md, "The native editor window is never
// parented to the FX strip") — the old `editors_` comment claimed a rebuild
// would not orphan it, which was true only of SPluginEffectStrip::rebuildUI()
// (the strip's OWN internal row rebuild, e.g. on reorder-plugin) and silent
// about the STrackDetailPanel one, which is the one that matters.
QHash<SPluginSlot *, QPointer<QDialog> > &genericEditorRegistry()
{
    static QHash<SPluginSlot *, QPointer<QDialog> > r;
    return r;
}

}  // namespace

SPluginEffectStrip::SPluginEffectStrip(STrack *track, QWidget *parent)
    : QWidget(parent), track_(track)
{
    pluginChain_ = track ? track->getPluginChain() : nullptr;

    setAcceptDrops(true);
    setMinimumHeight(100);  // Ensure plugin strip is always visible
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // Plugin list container with scroll area
    QWidget *scrollContent = new QWidget();
    scrollContent->setAcceptDrops(true);
    pluginsLayout_ = new QVBoxLayout(scrollContent);
    pluginsLayout_->setContentsMargins(4, 4, 4, 4);
    pluginsLayout_->setSpacing(4);

    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setWidget(scrollContent);
    scrollArea->setWidgetResizable(true);
    scrollArea->setMinimumHeight(80);  // Ensure scroll area is always visible
    mainLayout->addWidget(scrollArea, 1);  // Stretch factor 1

    // Add button
    QHBoxLayout *addLayout = new QHBoxLayout();
    addLayout->addStretch();
    addInstrumentBtn_ = new QPushButton("+ Add Instrument");
    addInstrumentBtn_->setMaximumWidth(200);
    addInstrumentBtn_->setToolTip(
        tr("Insert an instrument in slot 0. A track has at most one, and it is "
           "what sounds the track's MIDI clips."));
    addLayout->addWidget(addInstrumentBtn_);
    QPushButton *addBtn = new QPushButton("+ Add Effect");
    addBtn->setMaximumWidth(200);
    addLayout->addWidget(addBtn);
    mainLayout->addLayout(addLayout);

    connect(addBtn, &QPushButton::clicked, this, &SPluginEffectStrip::onAddPluginClicked);
    connect(addInstrumentBtn_, &QPushButton::clicked, this,
            &SPluginEffectStrip::onAddInstrumentClicked);

    // Connect chain signals if it exists
    if (pluginChain_) {
        connect(pluginChain_, &SPluginChain::slotInserted, this, &SPluginEffectStrip::onPluginSlotInserted);
        connect(pluginChain_, &SPluginChain::slotRemoved, this, &SPluginEffectStrip::onPluginSlotRemoved);
        // Reordering used to leave the strip showing the OLD order until
        // something else rebuilt it — the signal existed and nobody listened.
        connect(pluginChain_, &SPluginChain::slotsReordered, this,
                &SPluginEffectStrip::onPluginSlotsReordered);
    }

    // Initial build
    rebuildUI();
}

SPluginEffectStrip::~SPluginEffectStrip() = default;

// The strip's track, as an index-path from the root mixer ("2", "0,1", ...).
//
// This used to scan mixer->getTrackAt(i) over the mixer's DIRECT children only,
// so for a track NESTED inside a folder it found nothing and returned an empty
// string. Every caller here early-returns on empty — add, remove, bypass, edit
// and reorder alike — so the whole FX strip silently did nothing on a grouped
// track: the buttons were enabled, the clicks were accepted, and no action was
// ever submitted. Same defect family as the clip and fader verbs.
QString SPluginEffectStrip::trackPathString() const
{
    SProject *project = SApplication::app().getCurrentProject();
    if (!project) return QString();

    SObject *root = splacements::rootContainer(project);
    if (!root) return QString();

    // pathOf() returns {} for "the root itself" as well as "not found", but
    // track_ is an STrack and can never BE the root, so empty means not found.
    const QList<int> path = strackpath::pathOf(root, track_);
    if (path.isEmpty()) return QString();
    return strackpath::pathToString(path);
}

void SPluginEffectStrip::rebuildUI()
{
    // Clear existing widgets
    while (QLayoutItem *item = pluginsLayout_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    pluginWidgets_.clear();

    // Rebuild from plugin chain
    if (!pluginChain_) {
        return;
    }

    for (int i = 0; i < pluginChain_->getSlotCount(); ++i) {
        SPluginSlot *slot = pluginChain_->getSlotAt(i);
        if (!slot) continue;

        const audio::twPluginSlotState state = slot->getSlotState();
        const bool active = ( state == audio::twPluginSlotState::Active );

        // THE INSTRUMENT ROW COMES FIRST, and looks it (proposal 37 6.1). It is
        // first because it IS slot 0 - the position is the role (design D3) -
        // so this is a derived appearance, never a second ordering.
        const bool isInstrumentRow = slot->isInstrument();

        // Create a container widget for drag support.
        //
        // NO ROW SETS A BACKGROUND, and the instrument row is no exception.
        // It used to carry a pale "background: #efecf8", which is a
        // light-theme guess baked into a widget the theme cannot reach
        // (main/theme/CONTRACT.md names this file as one of the four sites
        // that carry their own setStyleSheet). Under the shipped DARK theme
        // the palette ink is #E6E0DD, so a near-white fill left the plugin
        // name and every control on the row light-grey-on-white and barely
        // readable. Both branches now set only a border and let the palette
        // paint the fill, so a row is readable under whatever theme is
        // active. The instrument is still distinguished FOUR other ways: the
        // accent border colour, the eighth-note prefix on its name, its extra
        // tooltip line, and kind=instrument in describeSlot().
        //
        // The selector is scoped to THIS widget's objectName (an ID selector),
        // never a bare "QWidget { ... }" type selector: Qt Style Sheets match a
        // type selector against every descendant that IS-A that type, so a
        // bare "QWidget { ... }" here would paint every child too - the bypass
        // checkbox, the Edit/Remove buttons, even the name label - because a
        // QPushButton/QCheckBox/QLabel all derive from QWidget.
        QWidget *container = new QWidget();
        container->setObjectName(QStringLiteral("pluginSlotRow"));
        container->setStyleSheet(
            isInstrumentRow
                ? "QWidget#pluginSlotRow { border: 1px solid #6050a0; "
                  "border-radius: 2px; padding: 4px; }"
                : "QWidget#pluginSlotRow { border: 1px solid #ccc; "
                  "border-radius: 2px; padding: 4px; }");
        // Not draggable: nothing may move in front of the instrument and the
        // instrument may not leave slot 0, so reorder-plugin would refuse the
        // drop anyway - better not to offer the grab.
        container->setCursor(isInstrumentRow ? Qt::ArrowCursor : Qt::OpenHandCursor);
        container->setAcceptDrops(true);
        QHBoxLayout *rowLayout = new QHBoxLayout(container);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        // Plugin name — the STORED one, so a missing plugin is identifiable.
        QString labelText = storedName(slot);
        if (isInstrumentRow) {
            labelText = tr("\xE2\x99\xAA ") + labelText;   // an eighth note
        }
        if (state == audio::twPluginSlotState::Missing) {
            labelText += tr(" (missing)");
        } else if (state == audio::twPluginSlotState::Unsupported) {
            labelText += tr(" (unsupported)");
        }
        QLabel *nameLabel = new QLabel(labelText);
        if (!active) {
            // Greyed, and greyed VISIBLY: setEnabled(false) alone is easy to miss
            // against the row's own stylesheet, so the colour is explicit.
            nameLabel->setEnabled(false);
            nameLabel->setStyleSheet("QLabel { color: #888888; border: none; }");
        }
        rowLayout->addWidget(nameLabel);

        // THE LATENCY BADGE (proposal 21 L5). Shown only when the plugin
        // reports one, so a chain of ordinary effects is unchanged on screen;
        // the number is REPORTED and nothing compensates for it (proposal 37 P9
        // owns plugin delay compensation), which is what the tooltip says.
        const std::uint32_t slotLatency = slot->reportedLatencyFrames();
        if (slotLatency > 0) {
            double rate = 48000.0;
            if (SProject *pr = SApplication::app().getCurrentProject())
                if (pr->getSRate() > 0) rate = pr->getSRate();
            QLabel *lat = new QLabel(
                QStringLiteral("%1 ms").arg(
                    QString::number(1000.0 * (double) slotLatency / rate, 'f', 1)));
            lat->setStyleSheet("QLabel { color: #a06000; border: none; }");
            lat->setToolTip(tr("This plugin reports %1 frames of latency. It is "
                               "NOT compensated: the live monitoring lane has no "
                               "delay line, so what you hear through it is late "
                               "by this much.").arg(slotLatency));
            rowLayout->addWidget(lat);
        }

        QString tooltip = reasonTooltip(slot);
        if (isInstrumentRow) {
            tooltip = tr("Instrument (slot 0): it sounds this track's MIDI clips "
                         "and its children's.\n") + tooltip;
        }
        container->setToolTip(tooltip);
        nameLabel->setToolTip(tooltip);

        // Bypass checkbox. Disabled on a non-Active slot: the placeholder is
        // already transparent, so the control would promise something it cannot
        // do. The stored flag is untouched and re-serialized as it was.
        QCheckBox *bypassCheckbox = new QCheckBox("Bypass");
        bypassCheckbox->setChecked(slot->getBypass());
        bypassCheckbox->setEnabled(active);
        bypassCheckbox->setToolTip(tooltip);
        rowLayout->addWidget(bypassCheckbox);

        // Edit (parameters). Double-clicking the row does the same thing; the
        // button exists because a double-click is not discoverable.
        QPushButton *editBtn = new QPushButton(tr("Edit"));
        editBtn->setMaximumWidth(70);
        editBtn->setEnabled(active);
        rowLayout->addWidget(editBtn);

        // Reload, only where it means something.
        QPushButton *reloadBtn = nullptr;
        if (!active) {
            reloadBtn = new QPushButton(tr("Reload"));
            reloadBtn->setMaximumWidth(80);
            reloadBtn->setToolTip(tooltip);
            rowLayout->addWidget(reloadBtn);
        }

        // Remove button
        QPushButton *removeBtn = new QPushButton("Remove");
        removeBtn->setMaximumWidth(80);
        rowLayout->addWidget(removeBtn);

        pluginsLayout_->addWidget(container);

        // Store widget pointers with index
        PluginWidget pw;
        pw.slotIndex = i;
        pw.slot = slot;
        pw.nameLabel = nameLabel;
        pw.bypassCheckbox = bypassCheckbox;
        pw.editBtn = editBtn;
        pw.reloadBtn = reloadBtn;
        pw.removeBtn = removeBtn;
        pw.container = container;
        pw.tooltip = tooltip;
        pluginWidgets_.push_back(pw);

        // Connect signals
        int slotIndex = i;
        connect(bypassCheckbox, &QCheckBox::toggled, this,
                [this, slotIndex](bool checked) { onPluginBypassToggled(slotIndex, checked); });
        connect(removeBtn, &QPushButton::clicked, this,
                [this, slotIndex]() { onRemovePluginClicked(slotIndex); });
        connect(editBtn, &QPushButton::clicked, this,
                [this, slotIndex]() { openParamEditor(slotIndex); });
        if (reloadBtn) {
            connect(reloadBtn, &QPushButton::clicked, this,
                    [this, slotIndex]() { onReloadPluginClicked(slotIndex); });
        }

        // Keep the checkbox honest when the flag moves in the MODEL (an undo of
        // set-plugin-bypass, a loaded project). Signals are blocked so syncing
        // the widget never submits another action.
        connect(slot, &SPluginSlot::bypassChanged, bypassCheckbox,
                [bypassCheckbox](bool b) {
                    if (bypassCheckbox->isChecked() == b) return;
                    const bool was = bypassCheckbox->blockSignals(true);
                    bypassCheckbox->setChecked(b);
                    bypassCheckbox->blockSignals(was);
                });

        // A reload can flip Missing -> Active, which changes the label, the
        // tooltip and which buttons exist. Deferred, because this fires from
        // inside the Reload button's own click handler. The CONTEXT is the
        // container, not `this`: the container dies with each rebuild, so the
        // connection dies with it — with `this` as context these would pile up
        // one per rebuild for the lifetime of the strip.
        connect(slot, &SPluginSlot::pluginReloaded, container, [this]() {
            QTimer::singleShot(0, this, [this]() { rebuildUI(); });
        });

        // An editor window already open on this slot must learn its NEW index:
        // reorder-plugin moves the slot without touching the dialog, and a stale
        // index would send the next parameter edit to a different plugin.
        if (QDialog *dlg = genericEditorRegistry().value(slot).data()) {
            if (SPluginParamEditor *ed =
                    dlg->findChild<SPluginParamEditor *>(QStringLiteral("paramEditor"))) {
                ed->setSlotIndex(i);
            }
        }

        // Double-click anywhere on the row opens the editor.
        container->installEventFilter(this);
        nameLabel->installEventFilter(this);
    }

    // THE CHAIN TOTAL (proposal 21 L5), below the rows and only when there is
    // one to report. Series inserts, so the delay is a plain sum.
    {
        const std::uint32_t total = chainLatencyFrames();
        if (total > 0) {
            QLabel *foot = new QLabel(tr("Chain latency: %1").arg(chainLatencyText()));
            foot->setObjectName(QStringLiteral("chainLatency"));
            foot->setStyleSheet("QLabel { color: #a06000; border: none; }");
            foot->setToolTip(tr("Reported by the plugins, NOT compensated. Plugin "
                                "delay compensation is not implemented."));
            pluginsLayout_->addWidget(foot);
        }
    }

    pluginsLayout_->addStretch();

    // Hidden once the track has one: a second instrument is refused, and an
    // affordance that only ever fails is worse than none.
    if (addInstrumentBtn_) addInstrumentBtn_->setVisible(addInstrumentEnabled());
}

bool SPluginEffectStrip::addInstrumentEnabled() const
{
    if (!pluginChain_ || pluginChain_->getSlotCount() <= 0) return true;
    SPluginSlot *first = pluginChain_->getSlotAt(0);
    return !(first && first->isInstrument());
}

QString SPluginEffectStrip::describeSlot(int slotIndex) const
{
    if (slotIndex < 0 || slotIndex >= (int) pluginWidgets_.size()) {
        return QString();
    }
    const PluginWidget &pw = pluginWidgets_[slotIndex];
    if (!pw.slot) return QString();
    // trackPath is here because it is the strip's single point of failure and
    // was invisible from outside: EVERY button handler resolves the track
    // through trackPathString() and early-returns when it comes back empty, so
    // a wrong answer disables the whole strip silently rather than misbehaving
    // visibly. It read empty for any track nested in a folder. Asserting the
    // resolved path is what makes that regression catchable.
    return QStringLiteral(
               // `kind` sits between mode and bypass DELIBERATELY: the M5
               // cases assert `name=...|state=...` and
               // `nameEnabled=...|...|reload=...` as contiguous spans, and a
               // field inserted inside either of those would break assertions
               // that are about something else entirely.
               "name=%1|state=%2|mode=%3|kind=%11|bypass=%4|nameEnabled=%5"
               "|bypassEnabled=%6|editEnabled=%7|reload=%8|trackPath=%9|tooltip=%10")
        .arg(pw.nameLabel->text(),
             QString::fromLatin1(stateName(pw.slot->getSlotState())),
             QString::fromLatin1(modeName(pw.slot->getSlotMode())))
        .arg(pw.slot->getBypass() ? 1 : 0)
        .arg(pw.nameLabel->isEnabled() ? 1 : 0)
        .arg(pw.bypassCheckbox->isEnabled() ? 1 : 0)
        .arg(pw.editBtn->isEnabled() ? 1 : 0)
        .arg(pw.reloadBtn ? 1 : 0)
        .arg(trackPathString())
        .arg(pw.tooltip)
        // DERIVED from the descriptor, like everything else about the role.
        .arg(pw.slot->isInstrument() ? QStringLiteral("instrument")
                                     : QStringLiteral("effect"))
        // APPENDED AT THE END on purpose (proposal 21 L5): the M5 cases assert
        // contiguous spans of the fields above, and a field inserted among them
        // would break assertions that are about something else entirely.
        + QStringLiteral("|latency=%1").arg(pw.slot->reportedLatencyFrames())
        // APPENDED AFTER latency=, for the same reason latency= was appended
        // after everything else (proposal 33 D2): the M5 cases assert
        // contiguous spans of the fields above and a field inserted among them
        // would break assertions about something else entirely. Model state,
        // like latency= and unlike every field before it - the flag lives in
        // the project and the row does not draw it.
        + QStringLiteral("|editorOpen=%1").arg(pw.slot->getEditorOpen() ? 1 : 0);
}

// THE CHAIN TOTAL (proposal 21 L5). A plain sum: every insert is in series, so
// the delay a chain adds is the sum of what its plugins report. NOTHING
// COMPENSATES FOR IT — plugin delay compensation is out of scope (proposal 37
// P9) — and on the LIVE lane in particular the pump renders block-wise with no
// delay line anywhere, so a latency-reporting plugin monitored live is heard
// late by exactly this much.
std::uint32_t SPluginEffectStrip::chainLatencyFrames() const
{
    std::uint32_t total = 0;
    if (!pluginChain_) return total;
    const int n = pluginChain_->getSlotCount();
    for (int i = 0; i < n; ++i)
        if (SPluginSlot *s = pluginChain_->getSlotAt(i))
            total += s->reportedLatencyFrames();
    return total;
}

QString SPluginEffectStrip::chainLatencyText() const
{
    const std::uint32_t total = chainLatencyFrames();
    double rate = 48000.0;
    if (SProject *p = SApplication::app().getCurrentProject())
        if (p->getSRate() > 0) rate = p->getSRate();
    return QStringLiteral("%1 frames (%2 ms)")
        .arg(total)
        .arg(QString::number(1000.0 * (double) total / rate, 'f', 1));
}

bool SPluginEffectStrip::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonDblClick) {
        for (size_t i = 0; i < pluginWidgets_.size(); ++i) {
            if (pluginWidgets_[i].container == watched ||
                pluginWidgets_[i].nameLabel == watched) {
                openParamEditor((int) i);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

SPluginParamEditor *SPluginEffectStrip::ensureParamEditor(int slotIndex,
                                                         bool showWindow)
{
    if (!pluginChain_) return nullptr;
    SPluginSlot *slot = pluginChain_->getSlotAt(slotIndex);
    if (!slot) return nullptr;

    const QString trackPath = trackPathString();
    if (trackPath.isEmpty()) {
        // Not a silent nothing (issue a, AC A3): a caller reaching this with
        // an empty path gets NO editor and no explanation, which is exactly
        // the "pressing Edit does nothing" shape the bug report described.
        // Reachable when track_ is not found under the project's MASTER root
        // (splacements::rootContainer() never resolves an arrangement) --
        // today that should not happen through the production Track Detail
        // dock (attachTrackDetail() follows only the master mixer's own
        // selectedTrackChanged), but a track removed from the tree between
        // the strip being built and Edit being pressed reaches it too.
        TW_LOGW( "pluginui",
                 "generic editor: track path for slot %d did not resolve (the "
                 "track is not reachable from the project's master root) -- "
                 "Edit has nothing to open", slotIndex );
        return nullptr;
    }

    // MODULE-LEVEL registry, not the old `editors_` member -- see
    // genericEditorRegistry()'s own comment for why.
    QDialog *dlg = genericEditorRegistry().value(slot).data();
    if (!dlg) {
        // Parented to window() -- the durable top-level ancestor -- exactly
        // as SPluginNativeEditor's constructor climbs past its own
        // parentForPosition. `this` (the strip) is never the Qt parent.
        dlg = new QDialog(window());
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(storedName(slot));
        dlg->resize(420, 320);
        QVBoxLayout *lay = new QVBoxLayout(dlg);
        lay->setContentsMargins(4, 4, 4, 4);
        SPluginParamEditor *editor =
            new SPluginParamEditor(slot, trackPath, slotIndex, dlg);
        editor->setObjectName(QStringLiteral("paramEditor"));
        lay->addWidget(editor);
        genericEditorRegistry().insert(slot, dlg);

        // The slot outliving its editor is the normal case; the reverse is not.
        // remove-plugin deletes the slot, so close the window with it rather than
        // leaving a dialog holding a dangling model pointer.
        //
        // SPluginSlot::slotDestroying(), NOT QObject::destroyed() -- the
        // SAME connection SPluginNativeEditor switched to (spluginslot.h's
        // own comment on the signal), for CONSISTENCY rather than a proven
        // crash here: unlike the native editor, SPluginParamEditor holds no
        // ABI twPluginEditor to detach() and its close path was verified NOT
        // to crash under the old destroyed()-based connection (a dedicated
        // probe script found no repro). destroyed() firing only after the
        // slot's plugin is already torn down is still the wrong signal to
        // build on, though -- SPluginParamEditor reads slot->getProcessor()
        // in several of its own slots (onParamsChanged(), onMeterTick()),
        // and nothing guarantees none of those can fire in the gap.
        connect(slot, &SPluginSlot::slotDestroying, dlg, &QDialog::close);

        // The registry entry outliving the dialog is a leak, not a crash (the
        // QPointer already reads null), but it would grow for as long as the
        // app runs and plugins get inserted/removed. Erase it when the dialog
        // actually goes -- `slot` is used only as a KEY here (pointer
        // identity), never dereferenced, so it is safe to capture even if the
        // slot itself is what triggered the close via the connection above.
        connect(dlg, &QObject::destroyed, dlg, [slot]() {
            genericEditorRegistry().remove(slot);
        } );
    }

    if (showWindow) {
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    }
    return dlg->findChild<SPluginParamEditor *>(QStringLiteral("paramEditor"));
}

bool SPluginEffectStrip::isGenericEditorOpenFor(SPluginSlot *slot)
{
    return genericEditorRegistry().value(slot).data() != nullptr;
}

void SPluginEffectStrip::closeGenericEditorFor(SPluginSlot *slot)
{
    if (QDialog *dlg = genericEditorRegistry().value(slot).data()) dlg->close();
}

void SPluginEffectStrip::openParamEditor(int slotIndex)
{
    // THE FALLBACK CHAIN (proposal 33, decision D3). A plugin with its own GUI
    // opens that and ONLY that: the slider list is not reachable alongside it,
    // which is why every native edit has to be undoable (M5) before this branch
    // may ship. Anything that cannot embed — no editor, a plugin that refuses
    // our window, an X11 run loop we do not provide yet — falls through to the
    // generic editor, which is a complete substitute and not an error state.
    if( pluginChain_ && track_ ) {
        if( SPluginSlot *slot = pluginChain_->getSlotAt( slotIndex ) ) {
            // The TRACK and the SLOT, not a path and an index: the window
            // outlives this strip and derives its own address at every commit
            // (see SPluginNativeEditor::openFor).
            if( SPluginNativeEditor::openFor( track_, slot, this ) )
                return;
        }
    }

    ensureParamEditor(slotIndex, /*showWindow=*/true);
}

QString SPluginEffectStrip::editorKindFor( int slotIndex )
{
    if( !pluginChain_ ) return QStringLiteral( "none" );
    SPluginSlot *slot = pluginChain_->getSlotAt( slotIndex );
    if( !slot ) return QStringLiteral( "none" );
    if( trackPathString().isEmpty() ) return QStringLiteral( "none" );

    // The SAME predicate openParamEditor() branches on, called rather than
    // restated: a second copy of the rule could drift from the one that runs.
    if( track_ && SPluginNativeEditor::isAvailableFor( slot ) )
        return QStringLiteral( "native" );

    // A non-Active slot has no parameters to show either; the strip disables
    // its Edit button for exactly that reason.
    if( slot->getSlotState() != audio::twPluginSlotState::Active )
        return QStringLiteral( "none" );

    return QStringLiteral( "generic" );
}

bool SPluginEffectStrip::editorSetParam(int slotIndex, std::uint32_t paramId,
                                       double value)
{
    // showWindow = false on purpose: see the header. A qxa run on Windows uses
    // the real platform plugin, and a test must not put a window on screen.
    SPluginParamEditor *editor = ensureParamEditor(slotIndex, /*showWindow=*/false);
    return editor && editor->setParamFromUi(paramId, value);
}

QString SPluginEffectStrip::editorValueText(int slotIndex, int row)
{
    SPluginParamEditor *editor = ensureParamEditor(slotIndex, /*showWindow=*/false);
    return editor ? editor->valueLabelText(row) : QString();
}

bool SPluginEffectStrip::ensureGenericEditorForTest(int slotIndex)
{
    return ensureParamEditor(slotIndex, /*showWindow=*/false) != nullptr;
}

void SPluginEffectStrip::onAddInstrumentClicked()
{
    SPluginBrowserDialog browser(this, SPluginBrowserDialog::Kind::Instruments);
    if (browser.exec() != QDialog::Accepted) return;
    const auto *descriptor = browser.selectedPlugin();
    if (!descriptor) return;
    const QString trackPath = trackPathString();
    if (trackPath.isEmpty()) return;
    // slotIndex 0 is what the caller MEANS; the action enforces it regardless
    // (an instrument descriptor always lands at 0, and a second is refused).
    SApplication::app().submitAction(
        new SInsertPluginAction(trackPath, 0, *descriptor));
}

void SPluginEffectStrip::onAddPluginClicked()
{
    SPluginBrowserDialog browser(this, SPluginBrowserDialog::Kind::Effects);
    if (browser.exec() == QDialog::Accepted) {
        const auto *descriptor = browser.selectedPlugin();
        if (descriptor) {
            const QString trackPath = trackPathString();
            if (trackPath.isEmpty()) return;
            int slotIndex = pluginChain_ ? pluginChain_->getSlotCount() : 0;
            SApplication::app().submitAction(
                new SInsertPluginAction(trackPath, slotIndex, *descriptor));
        }
    }
}

void SPluginEffectStrip::onRemovePluginClicked(int slotIndex)
{
    const QString trackPath = trackPathString();
    if (trackPath.isEmpty()) return;
    SApplication::app().submitAction(new SRemovePluginAction(trackPath, slotIndex));
}

void SPluginEffectStrip::onPluginBypassToggled(int slotIndex, bool bypassed)
{
    // Through the ACTION (proposal 08 M5). This used to be a direct
    // slot->setBypass(), which is the CONTRACT invariant 3 violation: the toggle
    // was not undoable and did not appear in a saved action script.
    const QString trackPath = trackPathString();
    if (trackPath.isEmpty()) return;
    SApplication::app().submitAction(
        new SSetPluginBypassAction(trackPath, slotIndex, bypassed));
}

void SPluginEffectStrip::onReloadPluginClicked(int slotIndex)
{
    if (!pluginChain_) return;
    SPluginSlot *slot = pluginChain_->getSlotAt(slotIndex);
    if (!slot) return;
    // Deliberately NOT an action. reloadPlugin() re-resolves the descriptor
    // against the registry and re-instantiates in place; it mutates no document
    // state (the descriptor and the state chunk are untouched, and a re-save
    // produces the same bytes), so there is nothing to undo. It is UI-thread
    // only, which is exactly where a button click is.
    slot->reloadPlugin();
}

void SPluginEffectStrip::onPluginSlotInserted(int index, SPluginSlot &slot)
{
    rebuildUI();
}

void SPluginEffectStrip::onPluginSlotRemoved(int index, SPluginSlot &slot)
{
    rebuildUI();
}

void SPluginEffectStrip::onPluginSlotsReordered()
{
    rebuildUI();
}

void SPluginEffectStrip::dragEnterEvent(QDragEnterEvent *event)
{
    // Accept drags from our own plugin containers
    if (event->source() == this || (event->source() && event->source()->parent() == this)) {
        event->acceptProposedAction();
    }
}

void SPluginEffectStrip::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->source() == this || (event->source() && event->source()->parent() == this)) {
        event->acceptProposedAction();
    }
}

void SPluginEffectStrip::dropEvent(QDropEvent *event)
{
    if (dragSourceIndex_ < 0) {
        return;
    }

    // Find the target position based on drop position
    QWidget *targetWidget = childAt(mapFromGlobal(QCursor::pos()));
    int targetIndex = -1;

    for (size_t i = 0; i < pluginWidgets_.size(); ++i) {
        if (pluginWidgets_[i].container == targetWidget ||
            pluginWidgets_[i].container->isAncestorOf(targetWidget)) {
            targetIndex = (int)i;
            break;
        }
    }

    if (targetIndex >= 0 && targetIndex != dragSourceIndex_) {
        // Through the ACTION. This used to call pluginChain_->reorderSlot()
        // directly — a model mutation with no undo entry (CONTRACT invariant 3).
        const QString trackPath = trackPathString();
        if (!trackPath.isEmpty()) {
            SApplication::app().submitAction(
                new SReorderPluginAction(trackPath, dragSourceIndex_, targetIndex));
        }
    }

    dragSourceIndex_ = -1;
    event->acceptProposedAction();
}
