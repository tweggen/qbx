#include "app/timeline/strackdetailpanel.h"
#include "app/timeline/ssubmit.h"
#include "app/timeline/sfadercurve.h"
#include "app/timeline/slevelmeter.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/model/sdefaultreset.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/ssolorules.h"
#include "app/pluginui/splugineffectstrip.h"
#include "app/timeline/sfeelflowpanel.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSlider>
#include <QLabel>
#include <QGuiApplication>
#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QKeyEvent>

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
    contentLayout_ = new QVBoxLayout(contentWidget_);
    contentLayout_->setContentsMargins(4, 4, 4, 4);
    contentLayout_->setSpacing(4);

    // Plugin strip (will be created when track is set)
    pluginStrip_ = nullptr;
    feelFlowPanel_ = nullptr;   // proposal 40 M3, mounted the same way

    // Shown instead of the content when no track is selected, so the panel is
    // never a large blank area (and the dock can shrink to it).
    placeholder_ = new QLabel(tr("No track selected"), this);
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setEnabled(false);
    mainLayout->addWidget(placeholder_);

    // THE SCROLL AREA IS THE FIX, and it is the same one SClipPropertiesPanel
    // already uses. A dock is free to be shorter than its contents; a LAYOUT is
    // not free to honour that by stacking its children on top of each other,
    // which is what this panel did. QScrollArea::widgetResizable resizes the
    // content to `viewport.expandedTo( qSmartMinSize( content ) )`, so the
    // section keeps its honest minimum and the overflow becomes a scrollbar.
    //
    // The two things that made it NOT honest, both deleted above: an explicit
    // 100 px minimum on contentWidget_ (an explicit minimum REPLACES the
    // layout's, it does not raise it), and a dead `pluginContainer` spacer that
    // was added here with stretch 1 and a 100 px minimum, never populated and
    // never removed — it competed for height with the strip that superseded it.
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidget(contentWidget_);
    // Transparent, so the panel's own #2a2a2a shows through rather than the
    // palette's Base colour: the scroll area is plumbing, not a surface.
    scroll_->viewport()->setAutoFillBackground(false);
    contentWidget_->setAutoFillBackground(false);
    mainLayout->addWidget(scroll_, 1);

    // Volume section. The slider uses the SHARED fader curve (sfadercurve.h) —
    // it previously did a naive `value = dB * 10`, which put the same dB at a
    // different pixel position than the arranger fader, and it was wired to
    // nothing at all, so dragging it silently did nothing.
    volumeRow_ = new QWidget(this);
    QHBoxLayout *volLayout = new QHBoxLayout(volumeRow_);
    volLayout->setContentsMargins(4, 2, 4, 4);
    volLayout->addWidget(new QLabel("Volume:"));
    volumeSlider_ = new QSlider(Qt::Horizontal);
    // Addressable by name from SMainWindow::doubleClickDetailControl, the same
    // reason the Clip Detail pane's fields carry object names.
    volumeSlider_->setObjectName(QStringLiteral("trackDetailVolumeSlider"));
    volumeSlider_->setMinimum(SFADER_MIN);
    volumeSlider_->setMaximum(SFADER_MAX);
    volumeSlider_->setSliderPosition(sDbToFader(0.0));
    // Home/End must always drive the transport, never this fader (item j) —
    // see the eventFilter() override below.
    volumeSlider_->installEventFilter(this);
    // Double-click = back to unity gain, the gesture the arranger track head's
    // fader has always had (SSMVMixerControl::eventFilter). setValue() emits
    // valueChanged, so the reset travels the SAME undoable path a drag does —
    // it is one SSetTrackVolumeAction, not a silent model write.
    //
    // EXACTLY 0.0 dB, and it commits DIRECTLY rather than through the slider's
    // valueChanged: the fader is an integer control and the curve's round trip
    // does not close on it — sDbToFader( 0.0 ) is tick -191, and sFaderToDb of
    // THAT is +0.0625 dB. Dragging cannot do better (a tick is a tick), but a
    // control asked to go back to unity must land ON unity. The arranger track
    // head's own double-click has always called applyVolume_( 0.0 ) for the
    // same reason; this is that, with the widget kept in step.
    sdefaultreset::onDoubleClick( volumeSlider_, [this] {
        const bool was = volumeSlider_->blockSignals( true );
        volumeSlider_->setValue( sDbToFader( 0.0 ) );
        volumeSlider_->blockSignals( was );
        applyVolumeDb( 0.0 );
    } );
    volumeSlider_->setToolTip(tr("Track volume. Double-click to reset to 0.0 dB."));
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
    // OUTSIDE the scroll area: the fader and the meter are the two things a
    // user watches while the transport runs, so scrolling the FX chain must
    // not carry them off the bottom of the dock.
    mainLayout->addWidget(volumeRow_, 0);

    connect(volumeSlider_, &QSlider::valueChanged,
            this, &STrackDetailPanel::onVolumeSliderMoved);
    connect(&SApplication::app(), &SApplication::meterTick,
            this, &STrackDetailPanel::onMeterTick);
    connect(&SApplication::app(), &SApplication::meterReset,
            meter_, &SLevelMeter::resetMeter);

    // Nothing to show until a track is selected.
    scroll_->setVisible(false);
    volumeRow_->setVisible(false);
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

// Home/End on the fader (item j): QAbstractSlider's own keyPressEvent maps
// them to "jump to minimum/maximum", so a fader that happens to have keyboard
// focus would silently steal the transport's go-to-start/-end shortcut.
// Caught here, BEFORE QSlider's handler ever sees them, and forwarded to the
// same SMainWindow slots the window-wide shortcuts use — mirrors
// SSMVMixerControl::eventFilter, which does the same thing for the arranger
// track head's fader.
bool STrackDetailPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == volumeSlider_ && event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Home || ke->key() == Qt::Key_End) {
            // NOT QApplication::activeWindow() — see
            // SSMVMixerControl::eventFilter's identical fix: a --test-case
            // run never shows the window, so nothing is ever "active" under
            // QT_QPA_PLATFORM=offscreen.
            SMainWindow *mw = nullptr;
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if ((mw = qobject_cast<SMainWindow*>(w))) break;
            }
            if (mw) {
                if (ke->key() == Qt::Key_Home) mw->gotoRangeStart();
                else                           mw->gotoRangeEnd();
            }
            return true;   // never let QAbstractSlider touch the fader's value
        }
    }
    return QWidget::eventFilter(watched, event);
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
    // Proposal 40 M3: same discipline -- deleted and re-created per track
    // switch, never owning anything across one (the track owns its own
    // Feel Flow state; this widget is a pure reader of it).
    if (feelFlowPanel_) {
        delete feelFlowPanel_;
        feelFlowPanel_ = nullptr;
    }

    if (currentTrack_) {
        // Create new plugin strip for this track, add directly to content
        pluginStrip_ = new SPluginEffectStrip(currentTrack_, this);
        pluginStrip_->setParent(contentWidget_);
        contentLayout_->insertWidget(0, pluginStrip_, 1);

        // Feel Flow section, below the FX strip and above the volume row.
        feelFlowPanel_ = new SFeelFlowPanel(currentTrack_, this);
        feelFlowPanel_->setParent(contentWidget_);
        contentLayout_->insertWidget(1, feelFlowPanel_, 0);

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

        scroll_->setVisible(true);
        volumeRow_->setVisible(true);
        placeholder_->setVisible(false);
    } else {
        probe_.setTap(nullptr);
        if (meter_) meter_->resetMeter();
        scroll_->setVisible(false);
        volumeRow_->setVisible(false);
        placeholder_->setVisible(true);
    }
    updateGeometry();   // the empty panel asks for far less room than a full one
}

void STrackDetailPanel::onVolumeSliderMoved(int sliderValue)
{
    applyVolumeDb( sFaderToDb( sliderValue ) );
}

// The commit, split out of the slider handler so the double-click reset can
// ask for a dB value the integer fader cannot spell exactly (see the ctor).
// Mirrors SSMVMixerControl::applyVolume_, which is split for the same reason.
void STrackDetailPanel::applyVolumeDb(double dB)
{
    if (!currentTrack_) return;

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
        stimeline::submitActive(new SSetTrackVolumeAction(trackPath, dB));
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
