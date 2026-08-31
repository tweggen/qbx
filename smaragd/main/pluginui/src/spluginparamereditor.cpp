#include "app/pluginui/spluginparamereditor.h"

#include "app/objects/track/spluginslot.h"
#include "app/objects/track/ssetpluginparamaction.h"
#include "app/shell/sapplication.h"
#include "app/shell/sautomationrecorder.h"
#include "app/model/sautomationlane.h"
#include "app/model/sdefaultreset.h"
#include "app/model/sobjectpath.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twpluginslotproc.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

namespace {

// The slider is an integer control, so every parameter is normalized onto a
// fixed tick count. 1000 ticks is 0.1% of the declared range — enough for a
// generic editor, and the ONLY place the quantisation lives (setParamFromUi()
// converts back through the same two helpers, so a scripted edit and a dragged
// edit land on identical values).
constexpr int kTicks = 1000;

int valueToTicks( const audio::twPluginParamInfo &info, double v )
{
    const double span = info.maxValue - info.minValue;
    if( span <= 0.0 ) return 0;
    double n = ( v - info.minValue ) / span;
    if( n < 0.0 ) n = 0.0;
    if( n > 1.0 ) n = 1.0;
    return (int) ( n * kTicks + 0.5 );
}

double ticksToValue( const audio::twPluginParamInfo &info, int ticks )
{
    const double span = info.maxValue - info.minValue;
    return info.minValue + ( ticks / (double) kTicks ) * span;
}

}  // namespace

SPluginParamEditor::SPluginParamEditor( SPluginSlot *slot,
                                        const QString &trackPath, int slotIndex,
                                        QWidget *parent )
    : QScrollArea( parent ), slot_( slot ), trackPath_( trackPath ),
      slotIndex_( slotIndex )
{
    setWidgetResizable( true );
    buildUI();

    if( slot_ ) {
        connect( slot_, &SPluginSlot::paramsChanged,
                 this, &SPluginParamEditor::onParamsChanged );
        connect( slot_, &SPluginSlot::pluginReloaded,
                 this, &SPluginParamEditor::onPluginReloaded );
    }
    // Proposal 37 P6: the READ display. Connected once per editor; the editor
    // is destroyed with its window, so the connection drops itself.
    connect( &SApplication::app(), &SApplication::meterTick,
             this, &SPluginParamEditor::onMeterTick );
}

audio::twPlugin *SPluginParamEditor::livePlugin() const
{
    if( !slot_ ) return nullptr;
    const auto &proc = slot_->getProcessor();
    // Bus 0's instance is the representative; a parameter edit is broadcast to
    // every instance by the action, so reading one is enough.
    return proc ? proc->plugin() : nullptr;
}

QString SPluginParamEditor::formatValue( const audio::twPluginParamInfo &info,
                                        double v ) const
{
    // Prefer the plugin's own formatting (units like dB/Hz/%, enum/choice names,
    // log scaling) — the only correct rendering for VST3's normalized domain. Fall
    // back to a numeric string when the plugin exposes no value-to-text.
    if( audio::twPlugin *p = livePlugin() ) {
        std::string t = p->paramValueText( info.id, v );
        if( !t.empty() )
            return QString::fromStdString( t );
    }
    return info.isStepped ? QString::number( (int) ( v + 0.5 ) )
                          : QString::number( v, 'f', 3 );
}

QString SPluginParamEditor::valueLabelText( int row ) const
{
    if( row < 0 || row >= (int) params_.size() )
        return {};
    return params_[row].valueLabel->text();
}

void SPluginParamEditor::buildUI()
{
    params_.clear();

    QWidget *container = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout( container );

    audio::twPlugin *plugin = livePlugin();
    if( !plugin ) {
        // A Missing/Unsupported slot (or one whose bus count was never declared)
        // legitimately has no parameters. Say so instead of showing an empty
        // scroll area the user cannot interpret.
        QLabel *empty = new QLabel(
            QObject::tr( "This plugin exposes no parameters." ) );
        empty->setEnabled( false );
        mainLayout->addWidget( empty );
        mainLayout->addStretch();
        setWidget( container );
        return;
    }

    const std::size_t paramCount = plugin->paramCount();
    params_.reserve( paramCount );

    for( std::size_t i = 0; i < paramCount; ++i ) {
        const audio::twPluginParamInfo info = plugin->paramInfo( i );

        QHBoxLayout *paramLayout = new QHBoxLayout();

        QLabel *nameLabel = new QLabel( QString::fromStdString( info.name ) );
        nameLabel->setMinimumWidth( 120 );
        paramLayout->addWidget( nameLabel );

        QSlider *slider = new QSlider( Qt::Horizontal );
        slider->setMinimum( 0 );
        slider->setMaximum( kTicks );
        slider->setValue( valueToTicks( info, plugin->getParam( info.id ) ) );
        // Double-click = the PLUGIN's own declared default, through the same
        // quantisation a drag goes through (valueToTicks), so the reset lands
        // on exactly the value a hand could have dragged to and reaches the
        // model through onParamSliderChanged like any other move — one
        // SSetPluginParamAction, or one tick of an automation pass.
        //
        // setParamFromUi() rather than slider->setValue(): a parameter already
        // AT its default quantises to the same tick, setValue() then emits
        // nothing, and the reset would silently do nothing on a control the
        // user may have moved between two values that share a tick. That seam
        // exists precisely to run the handler in that case.
        {
            const std::uint32_t pid = info.id;
            const double def = info.defaultValue;
            sdefaultreset::onDoubleClick( slider,
                                          [this, pid, def] { setParamFromUi( pid, def ); } );
        }
        slider->setToolTip(
            QObject::tr( "Double-click to reset to the plugin's default (%1)." )
                .arg( formatValue( info, info.defaultValue ) ) );
        paramLayout->addWidget( slider );

        QLabel *valueLabel = new QLabel();
        valueLabel->setMinimumWidth( 70 );
        valueLabel->setAlignment( Qt::AlignRight );
        valueLabel->setText( formatValue( info, plugin->getParam( info.id ) ) );
        paramLayout->addWidget( valueLabel );

        ParamWidget pw;
        pw.info       = info;
        pw.slider     = slider;
        pw.valueLabel = valueLabel;
        params_.push_back( pw );

        // Connect AFTER the initial setValue above, so building the UI never
        // submits an action for a value nobody moved.
        const int paramIndex = (int) i;
        connect( slider, &QSlider::valueChanged, this,
                 [this, paramIndex]() { onParamSliderChanged( paramIndex ); } );
        connect( slider, &QSlider::sliderReleased, this,
                 &SPluginParamEditor::onSliderReleased );

        mainLayout->addLayout( paramLayout );
    }

    mainLayout->addStretch();
    setWidget( container );
}

void SPluginParamEditor::onParamSliderChanged( int sliderIndex )
{
    if( applyingExternal_ ) return;
    if( sliderIndex < 0 || sliderIndex >= (int) params_.size() ) return;

    ParamWidget &pw = params_[sliderIndex];
    const audio::twPluginParamInfo &info = pw.info;
    const double value = ticksToValue( info, pw.slider->value() );

    pw.valueLabel->setText( formatValue( info, value ) );

    // A Touch/Latch/Write pass buffers on the UI thread and commits ONE
    // set-automation-points when the gesture ends (proposal 37 P6, D5), so a
    // slider drag during playback must not submit an action per tick. This is
    // ALSO the plugin-gesture punch-in: the app's own slider press/release is
    // the only gesture signal available, because a plugin's ParamGestureBegin/
    // End reaches the host only inside process(), on a worker thread, at freeze
    // time - and there is no native plugin editor to emit one (proposal 33 M3).
    SAutomationRecorder::Target t;
    // A qualified owner path names its own root; the recorder's Target
    // carries it (proposal 09 §3) so the pass commits in that arrangement.
    {
        const strackpath::QualifiedPath q_ = strackpath::parseQualified( trackPath_ );
        t.ownerPath = q_.idx;
        t.pathRoot  = q_.root;
    }
    t.target = QStringLiteral( "param:%1" ).arg( info.id );
    t.slotIndex = slotIndex_;
    if( SApplication::app().isPlaying()
        && SApplication::app().automationRecorder().writeTick(
               t, value, SApplication::app().getGlobalLocatorPos() ) )
        return;

    // THE action, not twPlugin::setParam(). The action broadcasts to every
    // instance, stales the slot's cached pages (without which the edit is
    // inaudible) and lands one coalesced entry on the undo stack per drag.
    SApplication::app().submitAction(
        new SSetPluginParamAction( trackPath_, slotIndex_, info.id, value ) );
}

void SPluginParamEditor::onParamsChanged()
{
    audio::twPlugin *plugin = livePlugin();
    if( !plugin ) return;

    // Re-read, do not rebuild: the parameter LIST is unchanged, only the values
    // moved (an undo, a restored state chunk, an automation write). Rebuilding
    // here would also destroy the slider the user is still dragging.
    applyingExternal_ = true;
    for( ParamWidget &pw : params_ ) {
        const double v = plugin->getParam( pw.info.id );
        pw.slider->setValue( valueToTicks( pw.info, v ) );
        pw.valueLabel->setText( formatValue( pw.info, v ) );
    }
    applyingExternal_ = false;
}

void SPluginParamEditor::onPluginReloaded()
{
    // A reload may have produced a DIFFERENT plugin (a Missing slot that became
    // Active, or a newer version of the same one), so the parameter list itself
    // has to be rebuilt — CONTRACT invariant 5.
    buildUI();
}

bool SPluginParamEditor::doubleClickParam( std::uint32_t paramId )
{
    for( ParamWidget &pw : params_ ) {
        if( pw.info.id != paramId ) continue;
        QSlider *slider = pw.slider;
        const QPointF c( slider->width() / 2.0, slider->height() / 2.0 );
        QMouseEvent dbl( QEvent::MouseButtonDblClick, c,
                         slider->mapToGlobal( c.toPoint() ), Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier );
        QCoreApplication::sendEvent( slider, &dbl );
        return true;
    }
    return false;
}

bool SPluginParamEditor::setParamFromUi( std::uint32_t paramId, double value )
{
    for( std::size_t i = 0; i < params_.size(); ++i ) {
        if( params_[i].info.id != paramId ) continue;
        QSlider *slider = params_[i].slider;
        const int ticks = valueToTicks( params_[i].info, value );
        if( slider->value() == ticks ) {
            // setValue() would not emit valueChanged, and the point of this seam
            // is to run the handler. Call it the way the signal would.
            onParamSliderChanged( (int) i );
        } else {
            slider->setValue( ticks );
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Automation (proposal 37 P6)
// ---------------------------------------------------------------------------

SAutomationLane *SPluginParamEditor::laneFor( std::uint32_t paramId ) const
{
    if( !slot_ ) return nullptr;
    return slot_->automationLane( QStringLiteral( "param:%1" ).arg( paramId ) );
}

void SPluginParamEditor::onSliderReleased()
{
    // Touch commits here; Latch and Write hold their value to the transport
    // stop. The recorder knows which - this is just "the hand let go".
    SApplication::app().automationRecorder().releaseControl();
}

void SPluginParamEditor::onMeterTick( offset_t pos, qint64 /*nowMs*/,
                                      bool /*live*/ )
{
    if( applyingExternal_ ) return;
    audio::twPlugin *plugin = livePlugin();
    if( !plugin ) return;

    SAutomationRecorder::Target t;
    // A qualified owner path names its own root; the recorder's Target
    // carries it (proposal 09 §3) so the pass commits in that arrangement.
    {
        const strackpath::QualifiedPath q_ = strackpath::parseQualified( trackPath_ );
        t.ownerPath = q_.idx;
        t.pathRoot  = q_.root;
    }
    t.slotIndex = slotIndex_;

    bool any = false;
    for( ParamWidget &pw : params_ ) {
        SAutomationLane *lane = laneFor( pw.info.id );
        if( !lane || !SAutomationRecorder::isReadFamily( lane->mode() ) ) continue;
        // A pass being RECORDED on this very lane shows the HAND, not the
        // curve - otherwise the display fights the finger dragging the slider.
        t.target = lane->target();
        if( SApplication::app().automationRecorder().isRecording( t ) ) continue;
        const double v = lane->valueAt( pos );
        const int ticks = valueToTicks( pw.info, v );
        if( pw.slider->value() == ticks ) continue;
        if( !any ) { applyingExternal_ = true; any = true; }
        pw.slider->setValue( ticks );
        pw.valueLabel->setText( formatValue( pw.info, v ) );
    }
    if( any ) applyingExternal_ = false;
}
