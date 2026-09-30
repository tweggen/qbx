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
#include <QShowEvent>
#include <QTimer>
#include <QtNumeric>
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
    // Climb to the durable TOP-LEVEL ancestor rather than owning through
    // whatever transient widget the caller happened to pass (see the header:
    // this is the fix for the window dying on every FX-strip rebuild). A
    // window is its own window(); nullptr's window() is nullptr.
    : QDialog( parent ? parent->window() : nullptr ), track_( track ), slot_( slot )
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
    // (splugineffectstrip.cpp) and the reason remove-plugin does not leave a
    // window addressing freed memory.
    //
    // slotDestroying(), NOT QObject::destroyed() (fixed 2026-08-23, found
    // chasing a SIGSEGV reproduced by opening a native editor and never
    // closing it before project teardown): destroyed() fires from ~QObject(),
    // the BASE class, which runs strictly AFTER ~SPluginSlot()'s body and
    // every member -- proc_, and the live audio::twPlugin instance behind it
    // -- has already been torn down, so close() -> closeEvent() ->
    // editor_->detach() reached a dangling extension pointer. slotDestroying()
    // is emitted first, while the slot and its plugin are still fully alive.
    if( slot_ ) {
        connect( slot_, &SPluginSlot::slotDestroying, this, &QDialog::close );
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

QSize SPluginNativeEditor::containerMinimumSizeFor( SPluginSlot *slot )
{
    SPluginNativeEditor *w = registry().value( slot ).data();
    if( !w || !w->container_ ) return QSize();
    // minimumSize(), not minimumSizeHint(): the hint is what the widget would
    // LIKE, and what QBX-119 turns on is what the layout will actually let the
    // user do. setFixedSize() sets this too, so a non-resizable editor answers
    // its one legal size here rather than nothing.
    return w->container_->minimumSize();
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

// The divisor between the plugin's geometry units and Qt's logical geometry.
//
// ONE on macOS, and that is the whole fix: VST3 (iplugview.h:98-100) and CLAP
// (gui.h) both state cocoa geometry in LOGICAL units, which are already the
// units QWidget geometry is in, so there is nothing to convert. Windows and X11
// state theirs in physical pixels, so those divide by the device pixel ratio.
//
// A dpr of zero is not a scale; it is a widget that has no window yet. Treat it
// as 1 rather than dividing by it.
qreal pluginToHostScale( qreal dpr )
{
#if defined( Q_OS_MACOS )
    Q_UNUSED( dpr );
    return 1.0;
#else
    return dpr > 0.0 ? dpr : 1.0;
#endif
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

// See the header for why these two are public and static, and why macOS is an
// identity rather than a scale.
QSize SPluginNativeEditor::hostSizeFor( audio::twEditorSize native, qreal dpr )
{
    const qreal s = pluginToHostScale( dpr );
    return QSize( qRound( native.width / s ), qRound( native.height / s ) );
}

audio::twEditorSize SPluginNativeEditor::nativeSizeFor( QSize host, qreal dpr )
{
    const qreal s = pluginToHostScale( dpr );
    return audio::twEditorSize{ qRound( host.width() * s ),
                                qRound( host.height() * s ) };
}

// See the header for why a rejected scale is silence rather than 1.0.
bool SPluginNativeEditor::isSendableScale( qreal scale )
{
    // qIsFinite rejects both NaN and the infinities. A NaN would otherwise pass
    // every comparison-based guard written the obvious way round: `scale > 0.0`
    // is false for NaN, but so is `scale <= 0.0`, and a guard spelled as the
    // negation of the second lets it straight through.
    return qIsFinite( scale ) && scale > 0.0;
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
        w->show();   // shown_ is set by showEvent(), which is also true for the
                     // already-open branch above
        w->raise();
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
    // at the right size rather than being corrected afterwards -- but only when
    // there is a scale to tell it. NOTHING HAS BEEN MAPPED AT THIS POINT: this
    // runs from openFor() before show(), so devicePixelRatioF() can still answer
    // 0, and 0 is not a scale (QBX-119; isSendableScale()).
    if( caps.scalable ) {
        const qreal scale = devicePixelRatioF();
        if( isSendableScale( scale ) )
            editor_->setScale( scale );
        else
            TW_LOGW( "pluginui",
                     "native editor: not sending a scale of %f -- this window has "
                     "none yet, and the plugin's own default beats a made-up one",
                     (double)scale );
    }

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

void SPluginNativeEditor::resizeToPlugin( audio::twEditorSize native )
{
    if( !native.valid() ) return;
    // Nothing of ours has that size: the plugin owns its floating window and
    // sizes it itself. Resizing this hidden dialog would be a no-op with a
    // layout pass attached.
    if( floating_ ) return;

    // THE ONE CONVERSION, and it happens exactly here and in resizeEvent()
    // below. twEditorSize is the PLUGIN'S OWN units (twplugineditor.h): physical
    // pixels on Windows and X11, logical points on macOS. Qt widget geometry is
    // logical, so the first two divide by the device pixel ratio and macOS does
    // not convert at all.
    //
    // Both halves of that have bitten. Skipping the division on Windows/X11 is a
    // no-op at 100% and a window the wrong size on every scaled display, which
    // never reproduces on the developer's own monitor. DOING it on macOS — where
    // the plugin was already speaking Qt's units — is what shipped, and it made
    // every native editor on a Retina Mac exactly half size with the plugin's
    // GUI cropped inside it.
    applyingResize_ = true;

    const QSize want = hostSizeFor( native, devicePixelRatioF() );

    // SIZE FIRST WITH THE BOUNDS PINNED, THEN RELAX THEM. THE ORDER IS THE FIX
    // FOR QBX-119, and it used to be the other way round.
    //
    // container_ is a bare QWidget, so its sizeHint() is (-1, -1): it has no
    // opinion about how big it ought to be. QWidgetItem::sizeHint() therefore
    // falls back to the widget's MINIMUM -- and the old code relaxed that
    // minimum to (0, 0) for a resizable plugin BEFORE calling adjustSize(). So
    // the layout's size hint was 0x0 and adjustSize() SHRANK THE WINDOW TO
    // NOTHING the moment a plugin said it could be resized. The plugin was then
    // handed a view with no area; an iPlug2/Skia editor turns that into a
    // CAMetalLayer drawable of 0x0 ("ignoring invalid setDrawableSize
    // width=0.000000 height=0.000000", verbatim in the ticket's log),
    // nextDrawable answers nil, and it dereferences the null SkCanvas that
    // follows -- the EXC_BAD_ACCESS inside SkCanvas::restoreToCount this ticket
    // was filed for.
    //
    // With setFixedSize() still in force adjustSize() has min == max == want to
    // read, which is a real hint, and the window comes up at the size the plugin
    // asked for. Only then may the bounds be widened, and the floor they are
    // widened to is the PLUGIN's own (minimumContainerSize()), never zero -- so
    // the user can drag but cannot drag the plugin down to nothing.
    //
    // Setting the size explicitly instead (container_->resize(); resize()) does
    // NOT work, and was measured not working: the dialog is still at its default
    // 640x480 at this point, a hidden top-level loses that resize to the next
    // layout pass, and the container came back stretched to 640x480 -- which
    // resizeEvent then forwarded to the plugin as a user resize.
    container_->setFixedSize( want );
    adjustSize();

    // Fixed only while the plugin says it cannot resize -- which is the common
    // path, every VST3 installed on this box reports canResize=no -- otherwise
    // the user is allowed to drag and resizeEvent negotiates.
    if( editor_ && editor_->caps().resizable ) {
        container_->setMinimumSize( minimumContainerSize() );
        container_->setMaximumSize( QWIDGETSIZE_MAX, QWIDGETSIZE_MAX );
    }

    applyingResize_ = false;
}

// The plugin's own floor, asked for in the one direction the resize-constraint
// protocol answers reliably: propose the smallest size that exists and take
// whatever comes back. A plugin with a real minimum rounds 1x1 up to it; one
// that does not implement the constraint echoes 1x1, which is already positive
// and already enough.
QSize SPluginNativeEditor::minimumContainerSize() const
{
    if( !editor_ ) return QSize( 1, 1 );

    audio::twEditorSize floor_ = editor_->constrain( audio::twEditorSize{ 1, 1 } );
    if( !floor_.valid() ) floor_ = audio::twEditorSize{ 1, 1 };

    const QSize s = hostSizeFor( floor_, devicePixelRatioF() );
    // hostSizeFor() DIVIDES on Windows and X11, so a 1x1 plugin floor rounds to
    // 0x0 there and would reintroduce exactly the zero this function exists to
    // forbid. The floor of the floor is 1.
    return QSize( qMax( 1, s.width() ), qMax( 1, s.height() ) );
}

void SPluginNativeEditor::showEvent( QShowEvent *e )
{
    // The one place "this window was really mapped" is true by construction.
    shown_ = true;
    QDialog::showEvent( e );
}

void SPluginNativeEditor::resizeEvent( QResizeEvent *e )
{
    QDialog::resizeEvent( e );
    if( applyingResize_ || !editor_ ) return;
    if( !editor_->caps().resizable ) return;

    // ONLY A WINDOW THE USER CAN ACTUALLY HAVE DRAGGED NEGOTIATES A SIZE.
    //
    // This handler exists for ONE caller: the user pulling the window's edges.
    // A resize the PLUGIN asked for arrives through poll()'s fb.resized and goes
    // to resizeToPlugin() instead, so nothing is lost by ignoring the rest --
    // and the rest is not harmless. A never-mapped dialog (the headless open, the
    // path every qxa case takes) still gets layout-driven resize events carrying
    // Qt's default 640x480 top-level geometry, delivered after applyingResize_
    // has gone back to false, because the layout is never reconciled against a
    // window that was never shown. Measured: with the container's minimum lifted
    // off zero, that event started telling tw.test.clap.gui it was 640x480 -- a
    // size nobody asked for, which the fixture duly reported back as a user
    // gesture and which turned the Gain its own GUI had just set back down.
    if( !shown_ ) return;

    // The inverse of resizeToPlugin()'s conversion, and it MUST be the inverse:
    // a round trip that does not land back where it started makes the plugin and
    // the container argue about the size, one poll at a time.
    audio::twEditorSize want = nativeSizeFor(
        QSize( container_->width(), container_->height() ), devicePixelRatioF() );

    // A NON-POSITIVE SIZE IS NEVER SENT (QBX-119). The container's minimum is
    // positive now, so arriving here with a zero extent means something other
    // than the user's drag produced it -- a layout pass during teardown, or a
    // dpr division that rounded a 1 down to 0 on Windows/X11. Either way the
    // plugin has no use for the number and at least one real plugin dies on it,
    // so the honest act is to DROP the resize rather than clamp it to a size the
    // user never asked for: clamping would fight the layout every 33 ms.
    if( !want.valid() ) {
        TW_LOGW( "pluginui",
                 "native editor: dropping a resize to %dx%d -- a plugin is never "
                 "told a non-positive size", want.width, want.height );
        return;
    }

    // Ask before telling: a plugin may only accept certain sizes, and onSize()
    // with one it refused is how a GUI ends up clipped.
    want = editor_->constrain( want );

    // ...AND THE ANSWER IS CHECKED TOO. constrain() is the PLUGIN's arithmetic,
    // returned verbatim by both backends (twvst3editor.cc, twclapeditor.cc), so
    // a plugin that rounds down to 0 puts us straight back where the guard above
    // started.
    if( !want.valid() ) {
        TW_LOGW( "pluginui",
                 "native editor: the plugin constrained a resize to %dx%d; "
                 "dropping it rather than forwarding a non-positive size",
                 want.width, want.height );
        return;
    }

    // AND A PLUGIN IS NEVER TOLD A SIZE IT ALREADY HAS. Every layout pass
    // delivers a resizeEvent, and most of them carry the size the plugin itself
    // just asked for, so forwarding one re-enters the plugin's GUI for nothing.
    // It is not merely wasteful: a plugin is entitled to read onSize() /
    // set_size() as something the USER did. tw.test.clap.gui queues a parameter
    // edit from it, which is how this was found -- an echoed resize turned the
    // Gain that the fixture's own GUI had just set straight back down again.
    const audio::twEditorSize have = editor_->size();
    if( want.width == have.width && want.height == have.height ) return;

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

    if( fb.restartRequested && slot_ ) handleRestart();
}

// A RESTART IS ONLY WORTH ACTING ON WHEN SOMETHING ACTUALLY CHANGED, and this
// guard is not defensive tidiness — without it, playback never starts.
//
// The measured chain, on an iPlug2-built plugin with its window open: a page
// arrives out of order, twPluginSlotProcessor resets the instance, the plugin
// re-reports its latency from OnReset() — which both SDK backends turn into a
// restart request UNCONDITIONALLY, whether or not the number moved — this poll
// sees it, notifyPluginEdited() stales the whole render path above the slot,
// every page is re-frozen out of order, and the reset happens again. The
// readahead can never reach its three seconds of buffer because its pages are
// staled as fast as it freezes them, so the transport waits forever.
//
// twvst3host.h closes the synchronous half of that loop at the source (a
// restart raised from inside a call the host itself made is not reported at
// all). This is the format-neutral half, and it is the one that catches CLAP:
// there, iPlug2 defers the request to the main thread, so it arrives long after
// the reset that provoked it and no scope at the source can recognise it.
//
// What the host consumes from a restart is METADATA — the latency badge and the
// parameter list. A parameter VALUE change does not come this way (VST3's
// kParamValuesChanged is filtered out in the backend; CLAP values arrive as
// out-events), so comparing those two is comparing everything a restart can
// tell us. Deliberately NOT rate-limiting instead: a slow loop is still a loop.
void SPluginNativeEditor::handleRestart()
{
    const long long latency = (long long)slot_->reportedLatencyFrames();
    const long long params  = (long long)slot_->paramRows().size();

    if( latency == lastRestartLatency_ && params == lastRestartParamCount_ ) {
        TW_LOGD( "pluginui", "native editor: restart with no observable change "
                 "(latency %lld, %lld params); not invalidating",
                 latency, params );
        return;
    }

    // The first restart after the window opens has nothing to compare against
    // and is acted on, which is correct: the host has not read the plugin's
    // configuration since it opened.
    lastRestartLatency_    = latency;
    lastRestartParamCount_ = params;

    // The slot's own consumers re-read on paramsChanged; this is the honest
    // place to say so.
    TW_LOGI( "pluginui", "native editor: plugin requested a restart "
             "(latency %lld, %lld params)", latency, params );
    slot_->notifyPluginEdited();
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
