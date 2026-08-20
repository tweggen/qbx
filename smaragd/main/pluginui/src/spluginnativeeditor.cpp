#include "app/pluginui/spluginnativeeditor.h"

#include "app/model/splacements.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/ssetpluginparamaction.h"
#include "app/shell/sapplication.h"
#include "app/shell/sautomationrecorder.h"
#include "app/shell/ssettings.h"
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

SPluginNativeEditor::SPluginNativeEditor( STrack *track, SPluginSlot *slot,
                                          QWidget *parent )
    : QDialog( parent ), track_( track ), slot_( slot )
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

bool SPluginNativeEditor::isOpenFor( SPluginSlot *slot )
{
    return registry().value( slot ).data() != nullptr;
}

void SPluginNativeEditor::closeFor( SPluginSlot *slot )
{
    if( SPluginNativeEditor *w = registry().value( slot ).data() )
        w->close();
}

namespace {

// Every track in the project, depth first. A folder's children are tracks in
// their own right and can hold plugins of their own.
void collectTracks( SObject *node, QList<STrack *> &out )
{
    if( !node ) return;
    for( SLink *lk : node->childLinks() ) {
        if( !lk ) continue;
        if( STrack *t = dynamic_cast<STrack *>( &lk->getSObject() ) ) {
            out.append( t );
            collectTracks( t, out );   // folders nest
        }
    }
}

}  // namespace

// See the header for why this is public and static.
QRect SPluginNativeEditor::clampOntoAScreen( const QRect &want )
{
    if( want.width() <= 0 || want.height() <= 0 ) return QRect();

    // The screen the window's own centre lands on, which is the one the user
    // last had it on. screenAt() answers null for a point on no screen at all -
    // exactly the unplugged case - and the primary is then the honest fallback.
    const QScreen *scr = QGuiApplication::screenAt( want.center() );
    if( !scr ) scr = QGuiApplication::primaryScreen();
    if( !scr ) return want;   // no screens at all: nothing to clamp against

    const QRect avail = scr->availableGeometry();
    QRect       r     = want;

    // Never grow a window past the screen; a plugin that asked for 1600x1200 on
    // a 1366x768 laptop is better clipped by us than by the window manager.
    r.setWidth( qMin( r.width(), avail.width() ) );
    r.setHeight( qMin( r.height(), avail.height() ) );

    if( r.right() > avail.right() )   r.moveRight( avail.right() );
    if( r.bottom() > avail.bottom() ) r.moveBottom( avail.bottom() );
    if( r.left() < avail.left() )     r.moveLeft( avail.left() );
    if( r.top() < avail.top() )       r.moveTop( avail.top() );
    return r;
}

QString SPluginNativeEditor::pluginKey() const
{
    if( !slot_ ) return QString();
    const audio::twPluginDescriptor &d = slot_->getDescriptor();
    if( d.uid.empty() ) return QString();
    return QString::fromStdString( d.format ) + QStringLiteral( ":" ) +
           QString::fromStdString( d.uid );
}

void SPluginNativeEditor::restoreGeometryFromSettings()
{
    if( floating_ ) return;   // the plugin owns that window, not us
    const QString key = pluginKey();
    if( key.isEmpty() ) return;

    const QRect stored = SSettings::instance().pluginEditorGeometry( key );
    if( !stored.isValid() ) return;

    const QRect r = clampOntoAScreen( stored );
    if( !r.isValid() ) return;

    // POSITION ALWAYS; SIZE ONLY IF THE PLUGIN CAN BE RESIZED. A fixed-size
    // editor has one correct size - the one it just told us in attachPlugin() -
    // and forcing a remembered one on it would clip its own drawing, silently,
    // for the rest of the session. Every VST3 installed on this box reports
    // canResize=no, so this is the common path rather than the exotic one.
    applyingResize_ = true;
    if( editor_ && editor_->caps().resizable )
        setGeometry( r );
    else
        move( r.topLeft() );
    applyingResize_ = false;
}

void SPluginNativeEditor::saveGeometryToSettings() const
{
    // Only a window that was really on screen has a geometry worth keeping. A
    // headless open (showWindow = false) and a floating editor both have none,
    // and storing theirs would overwrite the user's real position.
    if( !shown_ || floating_ ) return;
    const QString key = pluginKey();
    if( key.isEmpty() ) return;
    SSettings::instance().setPluginEditorGeometry( key, geometry() );
}

void SPluginNativeEditor::restoreOpenEditors( SProject *project,
                                              QWidget *parentForPosition )
{
    if( !project ) return;
    // See the header: a qxa run uses the real platform plugin, so this would put
    // plugin windows on the developer's screen mid-suite.
    if( SApplication::app().isTestCaseMode() ) return;

    QList<STrack *> tracks;
    collectTracks( splacements::rootContainer( project ), tracks );

    for( STrack *t : tracks ) {
        SPluginChain *chain = t ? t->getPluginChain() : nullptr;
        if( !chain ) continue;
        for( int i = 0; i < chain->getSlotCount(); ++i ) {
            SPluginSlot *slot = chain->getSlotAt( i );
            if( !slot || !slot->getEditorOpen() ) continue;
            // openFor() re-asserts the flag on success. On FAILURE it is left
            // exactly as the file had it, on purpose: the plugin may be missing
            // on this machine and back on the next one, and silently clearing
            // the flag would lose the user's setting to a temporary condition.
            SPluginNativeEditor::openFor( t, slot, parentForPosition );
        }
    }
}

SPluginNativeEditor *SPluginNativeEditor::openFor( STrack *track, SPluginSlot *slot,
                                                   QWidget *parentForPosition,
                                                   bool showWindow )
{
    if( !track || !slot ) return nullptr;

    // Already open: raise it rather than making a second one. Two views on one
    // instance is a lifetime problem the backend refuses anyway. Nothing to
    // re-point here — the address is derived at every commit.
    if( SPluginNativeEditor *existing = registry().value( slot ).data() ) {
        // A floating editor's window is the PLUGIN's, so there is nothing here
        // to raise; showing this one would put an empty rectangle beside it.
        if( !existing->isFloating() ) {
            existing->show();
            existing->raise();
            existing->activateWindow();
        }
        return existing;
    }

    if( !isAvailableFor( slot ) ) return nullptr;

    auto *w = new SPluginNativeEditor( track, slot, parentForPosition );
    if( !w->attachPlugin() ) {
        // Not a failure the user should see as an error: the generic editor is a
        // complete fallback and the caller opens it next.
        delete w;
        return nullptr;
    }

    registry().insert( slot, w );

    // D2: the flag travels in the project, so a save from here on says the
    // editor was open. Set BEFORE the show, so it is right even when the window
    // is opened headlessly and never mapped at all.
    slot->setEditorOpen( true );

    // ...and the geometry comes from SSettings, per user, clamped. After the
    // attach, because attachPlugin() has just sized the container to what the
    // plugin asked for, and this is allowed to override the position on top of
    // that.
    w->restoreGeometryFromSettings();

    // A floating editor has no content of ours to show; the plugin has already
    // put its own window on screen. This object stays alive, unshown, as the
    // poll pump and the registry entry.
    if( showWindow && !w->isFloating() ) {
        w->show();
        w->raise();
        w->shown_ = true;
    }
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
    if( h.valid() && editor_->attach( h ) ) {
        resizeToPlugin( editor_->size() );
        pollTimer_->start();
        return true;
    }

    // D1: OFFER THE FLOATING FORM WHEN EMBEDDING FAILS, before giving up on the
    // plugin's own interface altogether. CLAP is the only format that has one —
    // gui.h names it as "sometimes the only option due to technical
    // limitations" — and a plugin that declares it usually means it. The
    // transient parent is the APPLICATION window rather than this dialog:
    // ours is never shown, and a plugin window kept above an invisible parent
    // is a plugin window with nothing keeping it above the app.
    if( editor_->caps().floating ) {
        QWidget *top = parentWidget() ? parentWidget()->window() : nullptr;
        if( editor_->attachFloating( handleOf( top ) ) ) {
            floating_ = true;
            TW_LOGI( "pluginui",
                     "native editor: embedding refused, using the plugin's own "
                     "floating window" );
            pollTimer_->start();
            return true;
        }
    }

    editor_.reset();
    return false;
}

// --- geometry -----------------------------------------------------------------

void SPluginNativeEditor::resizeToPlugin( audio::twEditorSize physical )
{
    if( !physical.valid() ) return;
    // Nothing of ours has that size: the plugin owns its floating window and
    // sizes it itself. Resizing this hidden dialog would be a no-op with a
    // layout pass attached.
    if( floating_ ) return;

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
    // ...and the value the parameter held before the FIRST of them, which is
    // the only correct baseline for the undo entry. It cannot be read back
    // afterwards: poll() has already moved the plugin (twplugineditor.h,
    // twEditorParamEdit::previousValue).
    std::map<std::uint32_t, double> firstPrevOf;
    bool sawGestureEnd = false;
    for( const auto &e : fb.edits ) {
        if( e.phase == audio::twEditorGesture::Change ) {
            if( lastOf.find( e.paramId ) == lastOf.end() )
                firstPrevOf[e.paramId] = e.previousValue;
            lastOf[e.paramId] = e.value;
        }
        if( e.phase == audio::twEditorGesture::End )    sawGestureEnd = true;
    }
    for( const auto &kv : lastOf )
        applyEdit( kv.first, kv.second, firstPrevOf[kv.first], sawGestureEnd );

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

// --- the model address, derived ------------------------------------------------
//
// Both of these are the strip's own logic, and deliberately so: an edit from a
// native window must address the model exactly as the same edit from the strip
// would. Neither result is stored.

QString SPluginNativeEditor::currentTrackPath() const
{
    if( !track_ ) return QString();
    SProject *project = SApplication::app().getCurrentProject();
    if( !project ) return QString();
    SObject *root = splacements::rootContainer( project );
    if( !root ) return QString();
    // pathOf() answers {} for "the root itself" as well as "not found"; a track
    // is never the root, so an empty path here means the track has left the
    // project and there is nothing to address.
    return strackpath::pathToString( strackpath::pathOf( root, track_ ) );
}

int SPluginNativeEditor::currentSlotIndex() const
{
    if( !track_ || !slot_ ) return -1;
    SPluginChain *chain = track_->getPluginChain();
    if( !chain ) return -1;
    for( int i = 0; i < chain->getSlotCount(); ++i )
        if( chain->getSlotAt( i ) == slot_.data() ) return i;
    return -1;   // removed from the chain while the window was up
}

void SPluginNativeEditor::applyEdit( std::uint32_t paramId, double value,
                                     double previousValue, bool gestureEnd )
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
    // THE ADDRESS IS DERIVED HERE, at the moment of the commit, and never
    // cached. Removing a slot above this one renumbers it and moving the track
    // renumbers the path, both while this window is up; a stale pair would
    // commit the knob to a DIFFERENT PLUGIN. Empty/-1 means the track or the
    // slot is gone, and the honest answer is to drop the edit: the model has
    // nowhere to put it, and the window is about to close anyway.
    const QString trackPath = currentTrackPath();
    const int     slotIndex = currentSlotIndex();
    if( trackPath.isEmpty() || slotIndex < 0 ) return;

    SAutomationRecorder::Target t;
    // A qualified owner path names its own root; the recorder's Target
    // carries it (proposal 09 §3) so the pass commits in that arrangement.
    {
        const strackpath::QualifiedPath q_ = strackpath::parseQualified( trackPath );
        t.ownerPath = q_.idx;
        t.pathRoot  = q_.root;
    }
    t.target    = QStringLiteral( "param:%1" ).arg( paramId );
    t.slotIndex = slotIndex;

    SApplication &app = SApplication::app();
    if( app.isPlaying() &&
        app.automationRecorder().writeTick( t, value, app.getGlobalLocatorPos() ) ) {
        if( gestureEnd ) app.automationRecorder().releaseControl();
        return;
    }

    // 2. Otherwise the ordinary undoable verb, exactly as the app's own slider
    // submits it. Consecutive edits to one parameter collapse into one undo
    // entry through SSetPluginParamAction::mergeWith().
    // THE BASELINE TRAVELS WITH THE EDIT. Left to itself the action reads
    // getParam(), which poll() already moved to `value` — the inverse would
    // then restore the new value and undo would do nothing at all.
    auto *act = new SSetPluginParamAction( trackPath, slotIndex, paramId, value );
    act->setPreviousValue( previousValue );
    app.submitAction( act );

    if( gestureEnd ) app.automationRecorder().releaseControl();
}

// --- teardown -----------------------------------------------------------------

void SPluginNativeEditor::closeEvent( QCloseEvent *e )
{
    // D2, and in this order: the geometry belongs to the window and must be read
    // while it still has one; the flag belongs to the project.
    saveGeometryToSettings();
    // slot_ is a QPointer and this path is also reached FROM its destruction
    // (the destroyed -> close connection), so it can legitimately be null.
    //
    // A close driven by pluginReloaded clears the flag too. That is deliberate:
    // the window really is gone, and a project that claimed otherwise would
    // re-open an editor the user never asked for on the next load.
    if( slot_ ) slot_->setEditorOpen( false );

    if( pollTimer_ ) pollTimer_->stop();
    if( editor_ ) {
        editor_->detach();
        editor_.reset();
    }
    QDialog::closeEvent( e );
}
