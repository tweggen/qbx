// CLAP native editor — proposal 33 M6. See twclapeditor.h.

#include "twclapeditor.h"

#include "tw/core/twlog.h"

#include <cstdint>

namespace audio {

// --- construction -------------------------------------------------------------

const char *twClapEditor::apiString()
{
#if defined( _WIN32 )
    return CLAP_WINDOW_API_WIN32;
#elif defined( __APPLE__ )
    return CLAP_WINDOW_API_COCOA;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

twClapEditor::twClapEditor( const clap_plugin_t *plugin, const clap_plugin_gui_t *gui,
                            std::string title, DrainFn drain, ReleaseFn onDestroy )
    : plugin_( plugin ), gui_( gui ), title_( std::move( title ) ),
      drain_( std::move( drain ) ), onDestroy_( std::move( onDestroy ) )
{
    const char *api = apiString();

    // Asked BEFORE create(), which is step 1 of the gui.h flowchart and the only
    // order in which the answer means anything: create() takes the mode as an
    // argument, so the host has to have chosen it already.
    if( gui_ && gui_->is_api_supported ) {
        caps_.embeddable = gui_->is_api_supported( plugin_, api, false );
        caps_.floating   = gui_->is_api_supported( plugin_, api, true );
    }

    // set_scale is meaningless — gui.h says so in as many words — on the two
    // APIs that are specified in LOGICAL pixels. On win32 and x11 the sizes are
    // physical and the plugin has no other way to learn the monitor scale.
#if defined( __APPLE__ )
    caps_.scalable = false;
#else
    caps_.scalable = gui_ && gui_->set_scale != nullptr;
#endif

    // resizable is deliberately NOT set here. can_resize() is annotated
    // [main-thread & !floating] and sits at step 8 of the flowchart, i.e. AFTER
    // create(); asking now would be asking a plugin that has not allocated its
    // GUI yet. attach() fills it in, which is still before the host reads it —
    // the first read is the resize that follows a successful attach.

    // CLAP has no IRunLoop equivalent: a plugin embedded on X11 drives its own
    // repaints off the host's XEvent stream through the window it was parented
    // into. So needsRunLoop stays false on every platform, unlike the VST3
    // backend, and the Linux gap M8 covers is a VST3 gap only.
}

twClapEditor::~twClapEditor()
{
    // detach() first, ALWAYS, and for the same reason the VST3 editor does it:
    // the GUI must be destroyed before the instance that owns it, and the
    // plugin's live-editor count must not read zero while one still exists.
    detach();
    if( onDestroy_ ) onDestroy_();
}

twEditorApi twClapEditor::api() const
{
#if defined( _WIN32 )
    return twEditorApi::Win32Hwnd;
#elif defined( __APPLE__ )
    return twEditorApi::MacNSView;
#else
    return twEditorApi::X11Window;
#endif
}

// --- attach -------------------------------------------------------------------

namespace {

// Fill a clap_window_t from our tagged handle. Which union member is written is
// chosen by the API STRING, not by the build platform, so a handle tagged for a
// different platform than this build cannot be silently reinterpreted.
bool fillWindow( const twEditorHandle &h, clap_window_t &w )
{
    switch( h.api ) {
    case twEditorApi::Win32Hwnd:
        w.api   = CLAP_WINDOW_API_WIN32;
        w.win32 = h.handle;
        return true;
    case twEditorApi::MacNSView:
        w.api   = CLAP_WINDOW_API_COCOA;
        w.cocoa = h.handle;
        return true;
    case twEditorApi::X11Window:
        w.api = CLAP_WINDOW_API_X11;
        w.x11 = (clap_xwnd)(std::uintptr_t)h.handle;
        return true;
    default:
        return false;
    }
}

}  // namespace

bool twClapEditor::attach( const twEditorHandle &parent )
{
    if( !gui_ || !plugin_ || created_ ) return false;
    if( !caps_.embeddable ) return false;
    if( !parent.valid() || parent.api != api() ) {
        TW_LOGW( "plugins", "[clap] editor attach refused: handle api mismatch" );
        return false;
    }

    clap_window_t win{};
    if( !fillWindow( parent, win ) ) return false;

    if( !gui_->create || !gui_->create( plugin_, apiString(), false ) ) {
        TW_LOGW( "plugins", "[clap] gui->create(embedded) failed" );
        return false;
    }
    created_  = true;
    floating_ = false;

    // Step 8, and only now that the GUI exists. The host reads this immediately
    // after a successful attach, to decide whether the window may be dragged.
    caps_.resizable = gui_->can_resize && gui_->can_resize( plugin_ );

    if( !gui_->set_parent || !gui_->set_parent( plugin_, &win ) ) {
        TW_LOGW( "plugins", "[clap] gui->set_parent refused the host window" );
        detach();
        return false;
    }

    if( gui_->show && !gui_->show( plugin_ ) ) {
        // Not fatal on its own — some plugins paint on the first expose — but it
        // is the last thing that can go wrong, so say so rather than leaving a
        // blank rectangle unexplained.
        TW_LOGW( "plugins", "[clap] gui->show() returned false" );
    }
    shown_ = true;
    return true;
}

bool twClapEditor::attachFloating( const twEditorHandle &transientParent )
{
    if( !gui_ || !plugin_ || created_ ) return false;
    if( !caps_.floating ) return false;

    if( !gui_->create || !gui_->create( plugin_, apiString(), true ) ) {
        TW_LOGW( "plugins", "[clap] gui->create(floating) failed" );
        return false;
    }
    created_  = true;
    floating_ = true;

    // A floating window belongs to the plugin, so the host may not resize it and
    // must not ask. Saying so here is what keeps the app from calling setSize()
    // on something that has no host container at all.
    caps_.resizable = false;

    // Best effort, both of them: the transient relationship is what keeps the
    // plugin's window above the app rather than lost behind it, and the title is
    // what stops it reading "Untitled". Neither failing is a reason to give up a
    // window the user asked for.
    clap_window_t win{};
    if( transientParent.valid() && fillWindow( transientParent, win ) &&
        gui_->set_transient )
        gui_->set_transient( plugin_, &win );

    if( gui_->suggest_title && !title_.empty() )
        gui_->suggest_title( plugin_, title_.c_str() );

    if( gui_->show && !gui_->show( plugin_ ) ) {
        TW_LOGW( "plugins", "[clap] floating gui->show() failed" );
        detach();
        return false;
    }
    shown_ = true;
    return true;
}

void twClapEditor::detach()
{
    if( !gui_ || !plugin_ || !created_ ) return;

    // A GUI the plugin has already destroyed must NOT be hidden — that is a call
    // into a window that no longer exists — but destroy() must still run,
    // because gui.h makes it the ACKNOWLEDGEMENT of closed(was_destroyed=true)
    // rather than an optional cleanup.
    if( shown_ && !destroyedByPlugin_ && gui_->hide )
        gui_->hide( plugin_ );
    shown_ = false;

    if( gui_->destroy )
        gui_->destroy( plugin_ );
    created_ = false;
}

// --- geometry -----------------------------------------------------------------

twEditorSize twClapEditor::size() const
{
    twEditorSize s{};
    if( !gui_ || !plugin_ || !created_ || !gui_->get_size ) return s;
    std::uint32_t w = 0, h = 0;
    if( gui_->get_size( plugin_, &w, &h ) )
        s = twEditorSize{ (int)w, (int)h };
    // win32 and x11 are physical already; cocoa is logical and is converted by
    // the caller that knows the scale, exactly as the VST3 backend leaves it
    // (twplugineditor.h states the ABI rule).
    return s;
}

twEditorSize twClapEditor::constrain( twEditorSize want ) const
{
    if( !gui_ || !plugin_ || !created_ || !caps_.resizable || !gui_->adjust_size )
        return want;
    std::uint32_t w = (std::uint32_t)( want.width > 0 ? want.width : 1 );
    std::uint32_t h = (std::uint32_t)( want.height > 0 ? want.height : 1 );
    if( !gui_->adjust_size( plugin_, &w, &h ) )
        return want;
    return twEditorSize{ (int)w, (int)h };
}

bool twClapEditor::setSize( twEditorSize s )
{
    if( !gui_ || !plugin_ || !created_ || !caps_.resizable || !gui_->set_size )
        return false;
    if( !s.valid() ) return false;
    return gui_->set_size( plugin_, (std::uint32_t)s.width, (std::uint32_t)s.height );
}

bool twClapEditor::setScale( double factor )
{
    if( !gui_ || !plugin_ || !caps_.scalable || !gui_->set_scale ) return false;
    // Ignoring the call is EXPLICITLY allowed by gui.h, so false here is not an
    // error — the host treats it only as a reason not to retry.
    return gui_->set_scale( plugin_, factor );
}

// --- the poll -----------------------------------------------------------------

twEditorFeedback twClapEditor::poll()
{
    if( !drain_ ) return twEditorFeedback{};

    bool destroyed = false;
    twEditorFeedback fb = drain_( destroyed );
    if( destroyed ) {
        // Remember it for detach(), and make sure the host actually closes: a
        // plugin that destroyed its own window has closed itself whether or not
        // it also said so.
        destroyedByPlugin_ = true;
        shown_             = false;
        fb.closeRequested  = true;
    }
    return fb;
}

}  // namespace audio
