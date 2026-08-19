// VST3 native editor — proposal 33 M3. See twvst3editor.h.

#include "twvst3editor.h"

#include "tw/core/twlog.h"

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

using namespace Steinberg;

namespace audio {

// --- twVst3PlugFrame ----------------------------------------------------------

tresult PLUGIN_API twVst3PlugFrame::queryInterface( const TUID _iid, void **obj )
{
    // On Linux this must also answer Linux::IRunLoop or an X11 plugin never
    // repaints (proposal 33 §8, M8). Deliberately NOT claimed here: the rule
    // twvst3host.cc:226-228 already states is that saying yes to an interface we
    // do not implement is how a host earns a crash inside the plugin.
    return resolveOne( _iid, obj, IPlugFrame::iid );
}

tresult PLUGIN_API twVst3PlugFrame::resizeView( IPlugView *view, ViewRect *rect )
{
    if( !rect ) return kInvalidArgument;

    // Record and coalesce; the host resizes on its next poll. Calling back into
    // view->onSize() from HERE is what a naive frame does, and it is wrong twice:
    // the container has not actually been resized yet, so the plugin would be
    // told a size that is not true; and this can arrive from inside attached(),
    // where re-entering the view is exactly what some plugins do not survive.
    size_    = twEditorSize{ rect->getWidth(), rect->getHeight() };
    pending_ = true;
    (void)view;
    return kResultOk;
}

bool twVst3PlugFrame::takePending( twEditorSize &out )
{
    if( !pending_ ) return false;
    out      = size_;
    pending_ = false;
    return true;
}

// --- twVst3Editor -------------------------------------------------------------

twVst3Editor::twVst3Editor( IPlugView *view, twVst3ComponentHandler *handler,
                            ApplyFn apply, ReleaseFn onDestroy )
    : view_( view ), handler_( handler ), apply_( std::move( apply ) ),
      onDestroy_( std::move( onDestroy ) )
{
    // Capabilities are read ONCE, here, because the host asks before attaching
    // and a plugin's answers to these do not change over a view's life.
    caps_.embeddable = true;   // every VST3 editor is; there is no floating form
    caps_.floating   = false;
    caps_.resizable  = view_ && view_->canResize() == kResultTrue;

    if( view_ ) {
        IPlugViewContentScaleSupport *scale = nullptr;
        if( view_->queryInterface( IPlugViewContentScaleSupport::iid,
                                   (void **)&scale ) == kResultOk && scale ) {
            caps_.scalable = true;
            scale->release();
        }
    }

#if defined( __linux__ )
    // The plugin will ask our frame for an IRunLoop and we do not answer one
    // yet (M8). Declaring the need is what lets the host fall back to the
    // generic editor instead of showing a window that never repaints.
    caps_.needsRunLoop = true;
#endif
}

twVst3Editor::~twVst3Editor()
{
    // detach() first, ALWAYS: Steinberg requires removed() before the view is
    // released, and the frame must still be alive while removed() runs because
    // a plugin may call back into it on the way out. frame_ is a member, so it
    // outlives this body by construction — which is the reason it is a member
    // and not something the caller passes in.
    detach();
    if( view_ ) {
        view_->release();
        view_ = nullptr;
    }
    // Last, and after the view is gone: the plugin's live-editor count is what
    // its teardown checks, so it must not read zero while a view is still held.
    if( onDestroy_ ) onDestroy_();
}

twEditorApi twVst3Editor::api() const
{
#if defined( _WIN32 )
    return twEditorApi::Win32Hwnd;
#elif defined( __APPLE__ )
    return twEditorApi::MacNSView;
#else
    return twEditorApi::X11Window;
#endif
}

namespace {
FIDString platformTypeFor( twEditorApi a )
{
    switch( a ) {
        case twEditorApi::Win32Hwnd: return kPlatformTypeHWND;
        case twEditorApi::MacNSView: return kPlatformTypeNSView;
        case twEditorApi::X11Window: return kPlatformTypeX11EmbedWindowID;
        default:                     return nullptr;
    }
}
}  // namespace

bool twVst3Editor::attach( const twEditorHandle &parent )
{
    if( !view_ || attached_ ) return false;
    if( !parent.valid() || parent.api != api() ) {
        TW_LOGW( "plugins", "VST3 editor attach refused: handle api mismatch" );
        return false;
    }

    FIDString type = platformTypeFor( parent.api );
    if( !type ) return false;

    if( view_->isPlatformTypeSupported( type ) != kResultTrue ) {
        TW_LOGW( "plugins", "VST3 editor: plugin does not support platform type %s", type );
        return false;
    }

    // setFrame BEFORE attached(), and this ordering is MEASURED, not stylistic:
    // proposal 33 §4 recorded Dexed calling IPlugFrame::resizeView from INSIDE
    // attached(). With the frame installed afterwards that request is dropped
    // silently — no error, no log, just a plugin sized wrong for the rest of the
    // session.
    view_->setFrame( &frame_ );

    if( view_->attached( parent.handle, type ) != kResultOk ) {
        view_->setFrame( nullptr );
        TW_LOGW( "plugins", "VST3 editor: attached() refused the host window" );
        return false;
    }

    attached_ = true;
    return true;
}

void twVst3Editor::detach()
{
    if( !view_ ) return;
    if( attached_ ) {
        view_->removed();
        attached_ = false;
    }
    // Unconditionally, even if we were never attached: setFrame(&frame_) may
    // have run in a failed attach, and frame_ is about to die with this object.
    view_->setFrame( nullptr );
}

twEditorSize twVst3Editor::size() const
{
    twEditorSize s{};
    if( !view_ ) return s;
    ViewRect r{};
    if( view_->getSize( &r ) == kResultOk )
        s = twEditorSize{ r.getWidth(), r.getHeight() };
    // Physical pixels on Windows and X11 already; macOS is logical and is
    // converted by the caller that knows the scale (twplugineditor.h states the
    // ABI rule). Nothing to do here on the two physical platforms.
    return s;
}

twEditorSize twVst3Editor::constrain( twEditorSize want ) const
{
    if( !view_ || !caps_.resizable ) return want;
    ViewRect r{ 0, 0, want.width, want.height };
    if( view_->checkSizeConstraint( &r ) == kResultTrue )
        return twEditorSize{ r.getWidth(), r.getHeight() };
    return want;
}

bool twVst3Editor::setSize( twEditorSize s )
{
    if( !view_ || !attached_ || !caps_.resizable ) return false;
    ViewRect r{ 0, 0, s.width, s.height };
    return view_->onSize( &r ) == kResultOk;
}

bool twVst3Editor::setScale( double factor )
{
    if( !view_ || !caps_.scalable ) return false;
    IPlugViewContentScaleSupport *scale = nullptr;
    if( view_->queryInterface( IPlugViewContentScaleSupport::iid,
                               (void **)&scale ) != kResultOk || !scale )
        return false;
    const tresult r = scale->setContentScaleFactor( (float)factor );
    scale->release();
    return r == kResultOk;
}

twEditorFeedback twVst3Editor::poll()
{
    twEditorFeedback fb;
    if( !handler_ ) return fb;

    // 1. Parameter edits the plugin's own GUI made.
    //
    // Each one is applied to the mirror and the DSP HERE, on the main thread,
    // before the host sees it — so the audio follows a knob even if the host
    // does nothing with the feedback at all. The host's job with the returned
    // list is the MODEL half: undo and automation.
    //
    // §4.1: the phases are reported verbatim and are NOT drag boundaries. Dexed
    // brackets every single value in its own begin/end, so a host that made one
    // undo entry per gesture would make one per mouse step. Coalescing is the
    // host's business (SSetPluginParamAction::mergeWith), which is why nothing
    // is merged here — merging in the engine would destroy the information the
    // host needs to decide.
    for( const auto &e : handler_->takeEdits() ) {
        if( e.phase == twEditorGesture::Change && apply_ )
            apply_( e.id, e.value );
        fb.edits.push_back( twEditorParamEdit{ e.id, e.value, e.phase } );
    }

    // 2. A resize the plugin asked for, coalesced to the last one.
    twEditorSize newSize{};
    if( frame_.takePending( newSize ) ) {
        fb.resized = true;
        fb.newSize = newSize;
    }

    // 3. restartComponent. kParamValuesChanged alone means "re-read the values",
    // which the mirror refresh above already covers for anything the GUI told us
    // about; anything else means the host's whole picture of the plugin may be
    // stale. Reporting only the second kind keeps a routine setup-time
    // kParamValuesChanged (measured on Dexed, §4.1) from making the host rebuild
    // its parameter list on every open.
    const int32 flags = handler_->takeRestartFlags();
    if( flags & ~(int32)Vst::kParamValuesChanged )
        fb.restartRequested = true;

    return fb;
}

}  // namespace audio
