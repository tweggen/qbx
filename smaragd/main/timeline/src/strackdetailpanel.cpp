#include "app/timeline/strackdetailpanel.h"
#include "app/timeline/sfadercurve.h"
#include "app/timeline/slevelmeter.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/ssolorules.h"
#include "app/pluginui/splugineffectstrip.h"
#include "app/shell/sapplication.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

STrackDetailPanel::STrackDetailPanel(QWidget *parent)
    : QWidget(parent)
{
    // A QWidget subclass does NOT paint a style-sheet background on its own —
    // and declaring one suppresses the default palette fill, so without the
    // paintEvent below the panel's area is never written and keeps whatever
    // was last in the backing store (the stale black rectangle in the dock).
    // The selector is scoped to this class as well: an unqualified `QWidget`
    // rule cascades the background AND the border onto every child.
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("STrackDetailPanel { background-color: #2a2a2a; "
                  "border-top: 1px solid #555; }");

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

    // Shown instead of the content when no track is selected, so the panel is
    // never a large blank area (and the dock can shrink to it).
    placeholder_ = new QLabel(tr("No track selected"), this);
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setEnabled(false);
    mainLayout->addWidget(placeholder_);

    // Volume section. The slider uses the SHARED fader curve (sfadercurve.h) —
    // it previously did a naive `value = dB * 10`, which put the same dB at a
    // different pixel position than the arranger fader, and it was wired to
    // nothing at all, so dragging it silently did nothing.
    QHBoxLayout *volLayout = new QHBoxLayout();
    volLayout->addWidget(new QLabel("Volume:"));
    volumeSlider_ = new QSlider(Qt::Horizontal);
    volumeSlider_->setMinimum(SFADER_MIN);
    volumeSlider_->setMaximum(SFADER_MAX);
    volumeSlider_->setSliderPosition(sDbToFader(0.0));
    volLayout->addWidget(volumeSlider_);
    volumeLabel_ = new QLabel("+0.0 dB");
    volumeLabel_->setMinimumWidth(60);
    volLayout->addWidget(volumeLabel_);
    // Level meter for the selected track (proposal 34), beside its fader.
    meter_ = new SLevelMeter(this);
    // The dock is the ONE mount that grows with the project's width (proposal 36
    // B8): unlike a 120 px track head it has the room, so it meters every
    // channel at a readable thickness rather than dividing 8 px among six lanes.
    meter_->setGrowWithLanes(true);
    meter_->setOrientation(Qt::Horizontal);
    meter_->setMinimumWidth(72);
    meter_->setMaximumWidth(72);
    volLayout->addWidget(meter_);
    contentLayout_->addLayout(volLayout);  // Volume at bottom, no stretch

    connect(volumeSlider_, &QSlider::valueChanged,
            this, &STrackDetailPanel::onVolumeSliderMoved);
    connect(&SApplication::app(), &SApplication::meterTick,
            this, &STrackDetailPanel::onMeterTick);
    connect(&SApplication::app(), &SApplication::meterReset,
            meter_, &SLevelMeter::resetMeter);

    mainLayout->addWidget(contentWidget_, 1);  // Stretch factor 1 to use available space

    // Nothing to show until a track is selected.
    contentWidget_->setVisible(false);
}

// Required for the style sheet above to reach the screen: QWidget subclasses
// have to draw PE_Widget themselves.
void STrackDetailPanel::paintEvent(QPaintEvent *)
{
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
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

        // Update volume slider, through the shared curve so this fader and the
        // arranger's put the same dB in the same place.
        double volume = currentTrack_->getVolume();
        volumeSlider_->blockSignals(true);
        volumeSlider_->setValue(sDbToFader(volume));
        volumeSlider_->blockSignals(false);
        volumeLabel_->setText(QString::asprintf("%+.1f dB", volume));

        // Point the meter at THIS track's root component (its twRewire).
        probe_.setTap(currentTrack_->getRootComponent());
        if (meter_) meter_->resetMeter();

        contentWidget_->setVisible(true);
        placeholder_->setVisible(false);
    } else {
        probe_.setTap(nullptr);
        if (meter_) meter_->resetMeter();
        contentWidget_->setVisible(false);
        placeholder_->setVisible(true);
    }
    updateGeometry();   // the empty panel asks for far less room than a full one
}

void STrackDetailPanel::onVolumeSliderMoved(int sliderValue)
{
    if (!currentTrack_) return;

    const double dB = sFaderToDb(sliderValue);
    volumeLabel_->setText(QString::asprintf("%+.1f dB", dB));

    // Through the action system so the edit is undoable and both faders follow
    // the model, exactly as SSMVMixerControl::applyVolume_ does. Index-PATH, not
    // a top-level index: this used to scan the mixer's DIRECT children only, so
    // for a track nested in a folder it resolved -1 and this fader silently took
    // the non-undoable fallback below. That scan helper is now deleted.
    SProject *proj = SApplication::app().getCurrentProject();
    SObject *root = splacements::rootContainer(proj);
    const QList<int> trackPath =
        root ? strackpath::pathOf(root, currentTrack_) : QList<int>();
    if (!trackPath.isEmpty()) {
        SApplication::app().submitAction(new SSetTrackVolumeAction(trackPath, dB));
    } else {
        currentTrack_->setVolume(dB);
        if (SProject *p = SApplication::app().getCurrentProject())
            p->notifyArrangementChanged();
    }
}

void STrackDetailPanel::onMeterTick(offset_t pos, qint64 nowMs, bool live)
{
    if (!meter_ || !meter_->isVisible()) return;   // hidden dock does no work

    if (!live || !currentTrack_) { meter_->pushIdle(nowMs); return; }

    // THE shared rule (app/model/ssolorules.h), the same one the mixer routes by
    // and the track head applies — nested lanes included.
    SProject *proj = SApplication::app().getCurrentProject();
    if (!ssolo::isLaneAudible(splacements::rootContainer(proj), currentTrack_)) {
        meter_->pushIdle(nowMs);
        return;
    }

    // EVERY CHANNEL (proposal 36 B8). This is the mount that does not cap: the
    // dock has the room, so it is where a user sees all six lanes of a
    // six-channel project — and it is what the track head's tooltip points at
    // when it shows only the monitored pair.
    std::shared_ptr<twComponent> tap = currentTrack_->getRootComponent();
    probe_.setTap(tap);
    const int width = tap ? (int) tap->getOutputChannels() : 1;
    meter_->setLanes(width, width);            // no-op when unchanged

    twLevelSampleSet s;
    if (probe_.advanceTo(pos, s, width)) meter_->pushLevel(s, nowMs);
    else                                 meter_->pushIdle(nowMs);
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
    // With no track there is nothing to show, so do not reserve half the
    // screen for it — that empty reservation was most of the dead area in the
    // dock.
    if (!currentTrack_) return QSize(400, placeholder_->sizeHint().height() + 16);
    return QSize(400, preferredPanelHeight());
}
