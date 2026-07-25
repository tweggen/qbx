#include "app/timeline/strackdetailpanel.h"
#include "app/objects/track/strack.h"
#include "app/pluginui/splugineffectstrip.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QGuiApplication>
#include <QScreen>

STrackDetailPanel::STrackDetailPanel(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet("QWidget { background-color: #2a2a2a; border-top: 1px solid #555; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // No title row of our own: the dock's title bar carries the name, and its
    // close button (plus View -> Track detail) is the show/hide control.
    contentWidget_ = new QWidget();
    contentWidget_->setMinimumHeight(100);  // Ensure content area has minimum size
    contentLayout_ = new QVBoxLayout(contentWidget_);
    contentLayout_->setContentsMargins(4, 4, 4, 4);
    contentLayout_->setSpacing(4);

    // Plugin strip (will be created when track is set)
    pluginStrip_ = nullptr;

    // Spacer for plugins (will be populated when track is set)
    QWidget *pluginContainer = new QWidget();
    pluginContainer->setMinimumHeight(100);
    pluginContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    contentLayout_->addWidget(pluginContainer, 1);  // Gets extra space

    // Volume section
    QHBoxLayout *volLayout = new QHBoxLayout();
    volLayout->addWidget(new QLabel("Volume:"));
    volumeSlider_ = new QSlider(Qt::Horizontal);
    volumeSlider_->setMinimum(-960);
    volumeSlider_->setMaximum(240);
    volumeSlider_->setSliderPosition(0);
    volLayout->addWidget(volumeSlider_);
    volumeLabel_ = new QLabel("+0.0 dB");
    volumeLabel_->setMinimumWidth(60);
    volLayout->addWidget(volumeLabel_);
    contentLayout_->addLayout(volLayout);  // Volume at bottom, no stretch

    mainLayout->addWidget(contentWidget_, 1);  // Stretch factor 1 to use available space

    // Nothing to show until a track is selected.
    contentWidget_->setVisible(false);
}

STrackDetailPanel::~STrackDetailPanel() = default;

void STrackDetailPanel::setTrack(STrack *track)
{
    currentTrack_ = track;
    rebuildUI();
}

void STrackDetailPanel::rebuildUI()
{
    // Clear old plugin strip
    if (pluginStrip_) {
        delete pluginStrip_;
        pluginStrip_ = nullptr;
    }

    if (currentTrack_) {
        // Create new plugin strip for this track, add directly to content
        pluginStrip_ = new SPluginEffectStrip(currentTrack_, this);
        pluginStrip_->setParent(contentWidget_);
        contentLayout_->insertWidget(0, pluginStrip_, 1);

        // Update volume slider
        double volume = currentTrack_->getVolume();
        volumeSlider_->blockSignals(true);
        volumeSlider_->setValue((int)(volume * 10));
        volumeSlider_->blockSignals(false);
        volumeLabel_->setText(QString::asprintf("%+.1f dB", volume));

        contentWidget_->setVisible(true);
    } else {
        contentWidget_->setVisible(false);
    }
}

// Preferred height: 50% of screen height, but never more than 450px.
static int preferredPanelHeight()
{
    int screenHeight = 600;  // Default fallback
    if (QGuiApplication::primaryScreen()) {
        screenHeight = QGuiApplication::primaryScreen()->geometry().height();
    }
    return qMin(screenHeight / 2, 450);
}

QSize STrackDetailPanel::sizeHint() const
{
    return QSize(400, preferredPanelHeight());
}

int STrackDetailPanel::heightForWidth(int w) const
{
    return preferredPanelHeight();
}
