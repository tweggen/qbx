#include "strackdetailpanel.h"
#include "strack.h"
#include "splugineffectstrip.h"
#include "ssettings.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QScreen>

STrackDetailPanel::STrackDetailPanel(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet("QWidget { background-color: #2a2a2a; border-top: 1px solid #555; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Header with collapse/expand button
    QHBoxLayout *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(8, 4, 8, 4);
    expandCollapseBtn_ = new QPushButton("▾");
    expandCollapseBtn_->setMaximumWidth(24);
    expandCollapseBtn_->setMaximumHeight(20);
    headerLayout->addWidget(new QLabel("Track Detail"));
    headerLayout->addStretch();
    headerLayout->addWidget(expandCollapseBtn_);

    QWidget *headerWidget = new QWidget();
    headerWidget->setLayout(headerLayout);
    headerWidget->setStyleSheet("QWidget { background-color: #1a1a1a; border-bottom: 1px solid #444; }");
    mainLayout->addWidget(headerWidget);

    // Content widget (collapsible)
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

    connect(expandCollapseBtn_, &QPushButton::clicked, this, [this]() {
        setExpanded(!expanded_);
    });

    // Load saved state
    loadState();
}

STrackDetailPanel::~STrackDetailPanel() = default;

void STrackDetailPanel::setTrack(STrack *track)
{
    currentTrack_ = track;
    rebuildUI();
}

void STrackDetailPanel::setExpanded(bool expanded)
{
    if (expanded_ != expanded) {
        expanded_ = expanded;
        updateCollapsedState();
        saveState();
    }
}

void STrackDetailPanel::saveState()
{
    SSettings &settings = SSettings::instance();
    settings.setValue("TrackDetailPanel/Expanded", expanded_);
}

void STrackDetailPanel::loadState()
{
    SSettings &settings = SSettings::instance();
    expanded_ = settings.value("TrackDetailPanel/Expanded", true).toBool();
    updateCollapsedState();
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

        contentWidget_->setVisible(expanded_);
    } else {
        contentWidget_->setVisible(false);
    }
}

void STrackDetailPanel::updateCollapsedState()
{
    expandCollapseBtn_->setText(expanded_ ? "▾" : "▸");
    if (contentWidget_) {
        contentWidget_->setVisible(expanded_);
    }
    // Notify layout manager that our size has changed
    updateGeometry();
}

QSize STrackDetailPanel::sizeHint() const
{
    if (!expanded_) {
        // When collapsed, only report header height
        return QSize(400, 28);  // Header height + margins/borders
    }
    // When expanded: 50% of screen height, but max 450px
    int screenHeight = 600;  // Default fallback
    if (QGuiApplication::primaryScreen()) {
        screenHeight = QGuiApplication::primaryScreen()->geometry().height();
    }
    int maxHeight = qMin(screenHeight / 2, 450);
    return QSize(400, maxHeight);
}

int STrackDetailPanel::heightForWidth(int w) const
{
    if (!expanded_) {
        return 28;  // Header height only
    }
    // Return constrained height when expanded
    int screenHeight = 600;  // Default fallback
    if (QGuiApplication::primaryScreen()) {
        screenHeight = QGuiApplication::primaryScreen()->geometry().height();
    }
    return qMin(screenHeight / 2, 450);
}

void STrackDetailPanel::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
}

void STrackDetailPanel::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
}
