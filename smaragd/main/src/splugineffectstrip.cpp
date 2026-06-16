#include "splugineffectstrip.h"
#include "spluginchain.h"
#include "spluginslot.h"
#include "strack.h"
#include "spluginbrowserdialog.h"
#include "sapplication.h"
#include "sproject.h"
#include "sstdmixer.h"
#include "slink.h"
#include "actions/sinsertpluginaction.h"
#include "actions/sremovepluginaction.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>

SPluginEffectStrip::SPluginEffectStrip(STrack *track, QWidget *parent)
    : QWidget(parent), track_(track)
{
    pluginChain_ = track->getPluginChain();
    if (!pluginChain_) {
        return;
    }

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // Plugin list container with scroll area
    QWidget *scrollContent = new QWidget();
    pluginsLayout_ = new QVBoxLayout(scrollContent);
    pluginsLayout_->setContentsMargins(4, 4, 4, 4);
    pluginsLayout_->setSpacing(4);

    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setWidget(scrollContent);
    scrollArea->setWidgetResizable(true);
    mainLayout->addWidget(scrollArea);

    // Add button
    QHBoxLayout *addLayout = new QHBoxLayout();
    addLayout->addStretch();
    QPushButton *addBtn = new QPushButton("+ Add Effect");
    addBtn->setMaximumWidth(200);
    addLayout->addWidget(addBtn);
    mainLayout->addLayout(addLayout);

    connect(addBtn, &QPushButton::clicked, this, &SPluginEffectStrip::onAddPluginClicked);

    // Connect chain signals
    connect(pluginChain_, &SPluginChain::slotInserted, this, &SPluginEffectStrip::onPluginSlotInserted);
    connect(pluginChain_, &SPluginChain::slotRemoved, this, &SPluginEffectStrip::onPluginSlotRemoved);

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

        // Plugin row: name + bypass checkbox + remove button
        QHBoxLayout *rowLayout = new QHBoxLayout();

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

        pluginsLayout_->addLayout(rowLayout);

        // Store widget pointers
        PluginWidget pw;
        pw.nameLabel = nameLabel;
        pw.bypassCheckbox = bypassCheckbox;
        pw.removeBtn = removeBtn;
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
