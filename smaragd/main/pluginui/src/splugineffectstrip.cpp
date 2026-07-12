#include "app/pluginui/splugineffectstrip.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/pluginui/spluginbrowserdialog.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/model/slink.h"
#include "app/objects/track/sinsertpluginaction.h"
#include "app/objects/track/sremovepluginaction.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QCursor>

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
    QPushButton *addBtn = new QPushButton("+ Add Effect");
    addBtn->setMaximumWidth(200);
    addLayout->addWidget(addBtn);
    mainLayout->addLayout(addLayout);

    connect(addBtn, &QPushButton::clicked, this, &SPluginEffectStrip::onAddPluginClicked);

    // Connect chain signals if it exists
    if (pluginChain_) {
        connect(pluginChain_, &SPluginChain::slotInserted, this, &SPluginEffectStrip::onPluginSlotInserted);
        connect(pluginChain_, &SPluginChain::slotRemoved, this, &SPluginEffectStrip::onPluginSlotRemoved);
    }

    // Initial build
    rebuildUI();
}

SPluginEffectStrip::~SPluginEffectStrip() = default;

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

        // Create a container widget for drag support
        QWidget *container = new QWidget();
        container->setStyleSheet("QWidget { border: 1px solid #ccc; border-radius: 2px; padding: 4px; }");
        container->setCursor(Qt::OpenHandCursor);
        container->setAcceptDrops(true);
        QHBoxLayout *rowLayout = new QHBoxLayout(container);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        // Plugin name
        QLabel *nameLabel = new QLabel(slot->getSName());
        rowLayout->addWidget(nameLabel);

        // Bypass checkbox
        QCheckBox *bypassCheckbox = new QCheckBox("Bypass");
        bypassCheckbox->setChecked(slot->getBypass());
        rowLayout->addWidget(bypassCheckbox);

        // Remove button
        QPushButton *removeBtn = new QPushButton("Remove");
        removeBtn->setMaximumWidth(80);
        rowLayout->addWidget(removeBtn);

        pluginsLayout_->addWidget(container);

        // Store widget pointers with index
        PluginWidget pw;
        pw.slotIndex = i;
        pw.nameLabel = nameLabel;
        pw.bypassCheckbox = bypassCheckbox;
        pw.removeBtn = removeBtn;
        pw.container = container;
        pluginWidgets_.push_back(pw);

        // Connect signals
        int slotIndex = i;
        connect(bypassCheckbox, &QCheckBox::toggled, this,
                [this, slotIndex](bool checked) { onPluginBypassToggled(slotIndex, checked); });
        connect(removeBtn, &QPushButton::clicked, this,
                [this, slotIndex]() { onRemovePluginClicked(slotIndex); });
    }

    pluginsLayout_->addStretch();
}

void SPluginEffectStrip::onAddPluginClicked()
{
    SPluginBrowserDialog browser(this);
    if (browser.exec() == QDialog::Accepted) {
        const auto *descriptor = browser.selectedPlugin();
        if (descriptor) {
            // Find track index in the mixer
            SProject *project = SApplication::app().getCurrentProject();
            if (!project) return;

            SObject *root = project->getRootComponent();
            SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
            if (!mixer) return;

            int trackIdx = -1;
            for (int i = 0; i < mixer->getNTracks(); ++i) {
                SLink *link = mixer->getTrackAt(i);
                if (link && &link->getSObject() == track_) {
                    trackIdx = i;
                    break;
                }
            }
            if (trackIdx < 0) return;

            // Create path and action
            QString trackPath = QString("%1").arg(trackIdx);
            int slotIndex = pluginChain_->getSlotCount();
            SApplication::app().submitAction(
                new SInsertPluginAction(trackPath, slotIndex, *descriptor));
        }
    }
}

void SPluginEffectStrip::onRemovePluginClicked(int slotIndex)
{
    // Find track index in the mixer
    SProject *project = SApplication::app().getCurrentProject();
    if (!project) return;

    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) return;

    int trackIdx = -1;
    for (int i = 0; i < mixer->getNTracks(); ++i) {
        SLink *link = mixer->getTrackAt(i);
        if (link && &link->getSObject() == track_) {
            trackIdx = i;
            break;
        }
    }
    if (trackIdx < 0) return;

    // Create path and action
    QString trackPath = QString("%1").arg(trackIdx);
    SApplication::app().submitAction(new SRemovePluginAction(trackPath, slotIndex));
}

void SPluginEffectStrip::onPluginBypassToggled(int slotIndex, bool bypassed)
{
    SPluginSlot *slot = pluginChain_->getSlotAt(slotIndex);
    if (slot) {
        slot->setBypass(bypassed);
    }
}

void SPluginEffectStrip::onPluginSlotInserted(int index, SPluginSlot &slot)
{
    rebuildUI();
}

void SPluginEffectStrip::onPluginSlotRemoved(int index, SPluginSlot &slot)
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
        // Submit reorder action
        SProject *project = SApplication::app().getCurrentProject();
        if (!project) return;

        SObject *root = project->getRootComponent();
        SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
        if (!mixer) return;

        int trackIdx = -1;
        for (int i = 0; i < mixer->getNTracks(); ++i) {
            SLink *link = mixer->getTrackAt(i);
            if (link && &link->getSObject() == track_) {
                trackIdx = i;
                break;
            }
        }

        if (trackIdx >= 0 && pluginChain_) {
            pluginChain_->reorderSlot(dragSourceIndex_, targetIndex);
        }
    }

    dragSourceIndex_ = -1;
    event->acceptProposedAction();
}
