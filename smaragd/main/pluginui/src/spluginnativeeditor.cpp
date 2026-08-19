#include "app/pluginui/spluginnativeeditor.h"

#include "app/model/sobjectpath.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/ssetpluginparamaction.h"
#include "app/shell/sapplication.h"
#include "app/shell/sautomationrecorder.h"
#include "tw/core/twlog.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twpluginslotproc.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <map>

namespace {

// ~30 Hz, the same cadence SApplication::meterTick runs at and for the same
// reason: it is fast enough that a plugin-initiated resize does not visibly lag,
// and slow enough that draining a queue costs nothing.
constexpr int kPollMs = 33;

// One registry for the whole application, deliberately NOT owned by the FX strip
// (see the header). A QPointer per entry because the dialogs are
// WA_DeleteOnClose: an entry goes null by itself when the user closes a window,
// and nothing has to be unregistered from a destructor.
QHash<SPluginSlot *, QPointer<SPluginNativeEditor> > &registry()
{
    static QHash<SPluginSlot *, QPointer<SPluginNativeEditor> > r;
    return r;
}

audio::twPlugin *livePluginOf( SPluginSlot *slot )
{
    if( !slot ) return nullptr;
    const auto &proc = slot->getProcessor();
    // Bus 0's instance is the representative, exactly as SPluginParamEditor
    // reads it. Under DualMono there are N instances and only this one gets an
    // editor; the fan-out to the rest happens because every edit is committed
    // through SSetPluginParamAction, which writes all of them.
    return proc ? proc->plugin() : nullptr;
}

// A native window's handle, tagged for the ABI. Qt gives us a WId; what that
// integer MEANS is per-platform and the enum is how the engine finds out.
audio::twEditorHandle handleOf( QWidget *w )
{
    audio::twEditorHandle h;
    if( !w ) return h;
    const WId id = w->winId();   // forces a native window (WA_NativeWindow)
    if( !id ) return h;

#if defined( Q_OS_WIN )
    h.api = audio::twEditorApi::Win32Hwnd;
#elif defined( Q_OS_MACOS )
    h.api = audio::twEditorApi::MacNSView;
#else
    h.api = audio::twEditorApi::X11Window;
#endif
    h.handle = reinterpret_cast<void *>( id );
    return h;
}

}  // namespace

// --- construction -------------------------------------------------------------

SPluginNativeEditor::SPluginNativeEditor( SPluginSlot *slot, const QString &trackPath,
                                          int slotIndex, QWidget *parent )
    : QDialog( parent ), slot_( slot ), trackPath_( trackPath ), slotIndex_( slotIndex )
{
    setAttribute( Qt::WA_DeleteOnClose );
    setWindowTitle( slot_ ? slot_->getDescriptor().name.c_str() : "Plugin" );

    // No margins anywhere: the plugin's window must sit flush in the dialog, or
    // the size it asked for is not the size it gets.
    auto *lay = new QVBoxLayout( this );
    lay->setContentsMargins( 0, 0, 0, 0 );
    lay->setSpacing( 0 );

    container_ = new QWidget( this );
    // The load-bearing attribute. Without it the QWidget is a lightweight
    // Qt-internal rectangle with no platform window, winId() forces one anyway,
    // and the two disagree about geometry in ways that show up as a plugin drawn
    // at the wrong offset.
    container_->setAttribute( Qt::WA_NativeWindow );
    container_->setAttribute( Qt::WA_DontCreateNativeAncestors );
    lay->addWidget( container_ );

    pollTimer_ = new QTimer( this );
    pollTimer_->setInterval( kPollMs );
    connect( pollTimer_, &QTimer::timeout, this, &SPluginNativeEditor::onPoll );

    // The slot dying closes the window. Same rule the generic editor follows
    // (splugineffectstrip.cpp:524) and the reason remove-plugin does not leave a
    // window addressing freed memory.
    if( slot_ ) {
        connect( slot_, &QObject::destroyed, this, &QDialog::close );
        // A reload REPLACES every twPlugin instance (plugins/CONTRACT.md inv.
        // 18), so the view we hold points at a controller that is gone. There is
        // nothing to re-attach to in place: close, and let the user reopen.
        connect( slot_, &SPluginSlot::pluginReloaded, this, [this]() {
            TW_LOGI( "pluginui", "native editor closing: its plugin was reloaded" );
            close();
        } );
    }
}

SPluginNativeEditor::~SPluginNativeEditor()
{
    // ORDER IS THE CONTRACT (twplugineditor.h): stop polling, detach, destroy the
    // editor — and all of it before the plugin can be torn down. The plugin logs
    // an error if an editor outlives it, which is how a violation of this would
    // announce itself rather than becoming a rare crash.
    if( pollTimer_ ) pollTimer_->stop();
    if( editor_ ) {
        editor_->detach();
        editor_.reset();
    }
    registry().remove( slot_ );
}

// --- statics --------------------------------------------------------------

bool SPluginNativeEditor::isAvailableFor( SPluginSlot *slot )
{
    audio::twPlugin *p = livePluginOf( slot );
    return p && p->supportsNativeEditor();
}

void SPluginNativeEditor::repointSlotIndex( SPluginSlot *slot, int slotIndex )
{
    if( SPluginNativeEditor *w = registry().value( slot ).data() )
        w->slotIndex_ = slotIndex;
}

SPluginNativeEditor *SPluginNativeEditor::openFor( SPluginSlot *slot,
                                                   const QString &trackPath,
                                                   int slotIndex,
                                                   QWidget *parentForPosition )
{
    if( !slot || trackPath.isEmpty() ) return nullptr;

    // Already open: raise it rather than making a second one. Two views on one
    // instance is a lifetime problem the backend refuses anyway.
    if( SPluginNativeEditor *existing = registry().value( slot ).data() ) {
        existing->slotIndex_ = slotIndex;
        existing->show();
        existing->raise();
        existing->activateWindow();
        return existing;
    }

    if( !isAvailableFor( slot ) ) return nullptr;

    auto *w = new SPluginNativeEditor( slot, trackPath, slotIndex, parentForPosition );
    if( !w->attachPlugin() ) {
        // Not a failure the user should see as an error: the generic editor is a
        // complete fallback and the caller opens it next.
        delete w;
        return nullptr;
    }

    registry().insert( slot, w );
    w->show();
    w->raise();
    return w;
}

// --- attach -------------------------------------------------------------------

bool SPluginNativeEditor::attachPlugin()
{
    audio::twPlugin *p = livePluginOf( slot_ );
    if( !p ) return false;

    editor_ = p->createEditor();
    if( !editor_ ) return false;

    const audio::twEditorCaps caps = editor_->caps();

    // A plugin that needs a run loop we do not provide would open a window that
    // never repaints — worse than no window at all, because it looks like the
    // app hung. Refuse and fall back. (X11 only; M8.)
    if( caps.needsRunLoop ) {
        TW_LOGW( "pluginui",
                 "native editor declined: the plugin needs an X11 run loop, which "
                 "is not implemented yet (proposal 33 M8). Falling back to sliders." );
        editor_.reset();
        return false;
    }

    // Tell the plugin the monitor scale BEFORE attaching, so its first paint is
    // at the right size rather than being corrected afterwards.
    if( caps.scalable )
        editor_->setScale( devicePixelRatioF() );

    const audio::twEditorHandle h = handleOf( container_ );
    if( !h.valid() || !editor_->attach( h ) ) {
        editor_.reset();
        return false;
    }

    resizeToPlugin( editor_->size() );
    pollTimer_->start();
    return true;
}

// --- geometry -----------------------------------------------------------------

void SPluginNativeEditor::resizeToPlugin( audio::twEditorSize physical )
{
    if( !physical.valid() ) return;

    // THE ONE CONVERSION. twEditorSize is PHYSICAL PIXELS by ABI fiat
    // (twplugineditor.h), because VST3 coordinates are physical on Windows and
    // X11 but LOGICAL on macOS — the backend normalizes, and everything above it
    // sees one convention. Qt widget geometry is logical, so the physical size
    // is divided by the device pixel ratio exactly here and nowhere else.
    // Skipping it is a no-op at 100% and a window the wrong size on every
    // scaled display, which is the classic embedding bug and never reproduces on
    // the developer's own monitor.
    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const int   w   = qRound( physical.width / dpr );
    const int   hgt = qRound( physical.height / dpr );

    applyingResize_ = true;
    container_->setFixedSize( w, hgt );
    // Fixed only while the plugin says it cannot resize; otherwise the user is
    // allowed to drag and resizeEvent negotiates.
    if( editor_ && editor_->caps().resizable )
        container_->setMinimumSize( 0, 0 ), container_->setMaximumSize( 16777215, 16777215 );
    adjustSize();
    applyingResize_ = false;
}

void SPluginNativeEditor::resizeEvent( QResizeEvent *e )
{
    QDialog::resizeEvent( e );
    if( applyingResize_ || !editor_ ) return;
    if( !editor_->caps().resizable ) return;

    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    audio::twEditorSize want{ qRound( container_->width() * dpr ),
                              qRound( container_->height() * dpr ) };

    // Ask before telling: a plugin may only accept certain sizes, and onSize()
    // with one it refused is how a GUI ends up clipped.
    want = editor_->constrain( want );
    editor_->setSize( want );
}

// --- the poll -----------------------------------------------------------------

void SPluginNativeEditor::onPoll()
{
    if( !editor_ ) return;

    audio::twEditorFeedback fb = editor_->poll();

    // Coalesce WITHIN the batch, per parameter (proposal 33 §4.2). poll() may
    // report several moves of one knob — a drag moves it more than once inside
    // 33 ms — and submitting an action per reported edit would put several on
    // the undo stack per tick. The value that matters is the last one; the
    // intermediates have already reached the DSP inside poll(), which is what
    // keeps the drag sounding continuous.
    std::map<std::uint32_t, double> lastOf;
    bool sawGestureEnd = false;
    for( const auto &e : fb.edits ) {
        if( e.phase == audio::twEditorGesture::Change ) lastOf[e.paramId] = e.value;
        if( e.phase == audio::twEditorGesture::End )    sawGestureEnd = true;
    }
    for( const auto &kv : lastOf )
        applyEdit( kv.first, kv.second, sawGestureEnd );

    if( sawGestureEnd && lastOf.empty() ) {
        // An End with no value of its own still ends a recording gesture.
        SApplication::app().automationRecorder().releaseControl();
    }

    if( fb.resized ) resizeToPlugin( fb.newSize );

    if( fb.closeRequested ) {
        close();
        return;
    }

    if( fb.restartRequested ) {
        // The plugin's parameter list, I/O or latency changed. The slot's own
        // consumers re-read on paramsChanged; this is the honest place to say so.
        TW_LOGI( "pluginui", "native editor: plugin requested a restart" );
        if( slot_ ) slot_->notifyPluginEdited();
    }
}

void SPluginNativeEditor::applyEdit( std::uint32_t paramId, double value, bool gestureEnd )
{
    if( !slot_ ) return;

    // ECHO GUARD, and it is not hypothetical. Committing an edit runs
    // SSetPluginParamAction -> twPlugin::setParam(), whose VST3 implementation
    // calls controller_->setParamNormalized() so the plugin's own GUI agrees
    // with the audio. A plugin that reports THAT write back as a performEdit
    // would hand us the same value on the next poll, we would commit it again,
    // and the two would push each other round the loop for as long as the window
    // is open — at 30 Hz, filling the undo stack.
    //
    // Dropping a value identical to the one we last committed breaks the cycle
    // and costs nothing: re-committing a value the model already holds is a
    // no-op with an undo entry attached. A genuine user move to the same value
    // is, by definition, not a change.
    auto it = lastCommitted_.find( paramId );
    if( it != lastCommitted_.end() && it->second == value ) {
        if( gestureEnd ) SApplication::app().automationRecorder().releaseControl();
        return;
    }
    lastCommitted_[paramId] = value;

    // THE AUDIO HAS ALREADY FOLLOWED. twVst3Editor::poll() wrote the mirror and
    // the DSP ring on its way through, so what happens here is purely the MODEL
    // half: undo, and automation recording. That split is why a plugin GUI stays
    // responsive even while the host is busy.

    // 1. A Touch/Latch/Write pass takes the value instead — the punch-in that
    // pluginui/CONTRACT.md inv. 9 records as impossible ("there is no native
    // plugin editor to raise one"). It is possible now, and this is it.
    SAutomationRecorder::Target t;
    t.ownerPath = strackpath::stringToPath( trackPath_ );
    t.target    = QStringLiteral( "param:%1" ).arg( paramId );
    t.slotIndex = slotIndex_;

    SApplication &app = SApplication::app();
    if( app.isPlaying() &&
        app.automationRecorder().writeTick( t, value, app.getGlobalLocatorPos() ) ) {
        if( gestureEnd ) app.automationRecorder().releaseControl();
        return;
    }

    // 2. Otherwise the ordinary undoable verb, exactly as the app's own slider
    // submits it. Consecutive edits to one parameter collapse into one undo
    // entry through SSetPluginParamAction::mergeWith().
    app.submitAction( new SSetPluginParamAction( trackPath_, slotIndex_, paramId, value ) );

    if( gestureEnd ) app.automationRecorder().releaseControl();
}

// --- teardown -----------------------------------------------------------------

void SPluginNativeEditor::closeEvent( QCloseEvent *e )
{
    if( pollTimer_ ) pollTimer_->stop();
    if( editor_ ) {
        editor_->detach();
        editor_.reset();
    }
    QDialog::closeEvent( e );
}
