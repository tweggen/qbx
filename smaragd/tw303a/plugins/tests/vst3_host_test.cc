// vst3_host_test - the host objects a VST3 plugin talks to, driven directly.
//
// QBX-313: IComponentHandler2::setDirty is how a VST3 plugin says "state you
// save for me changed, and no parameter did" (NassauEQ's analyzer mode). The
// host's handler must OFFER the interface, REPORT setDirty(true) to the editor
// poll, and NOT report one raised from inside a call the host made (setState:
// a plugin answering a project load is not a modification).
//
// There is no headless route to a VST3 editor poll (twtestvst3.cpp's
// createView returns nullptr), so the qxa case plugin_state_dirty_marks_
// modified gates the end-to-end path through the CLAP fixture, and this gates
// the VST3 handler that feeds twVst3Editor::poll()'s stateDirty.

#include "twvst3host.h"

#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <iostream>
#include <thread>

using namespace Steinberg;

namespace {

int g_failures = 0;

bool check( bool cond, const char *what )
{
    std::cout << ( cond ? "  [PASS] " : "  [FAIL] " ) << what << std::endl;
    if( !cond ) ++g_failures;
    return cond;
}

}  // namespace

int main()
{
    std::cout << "=== VST3 host: IComponentHandler2::setDirty (QBX-313) ===" << std::endl;

    audio::twVst3ComponentHandler handler;
    Vst::IComponentHandler *h1 = &handler;

    // 1. Offered: through queryInterface (what EditController::
    // setComponentHandler asks) and through IPlugInterfaceSupport.
    Vst::IComponentHandler2 *h2 = nullptr;
    const tresult qr = h1->queryInterface( Vst::IComponentHandler2::iid, (void **)&h2 );
    if( !check( qr == kResultOk && h2 != nullptr,
                "the handler answers queryInterface(IComponentHandler2)" ) )
        return 1;
    check( (void *)h2 == (void *)static_cast<Vst::IComponentHandler2 *>( &handler ),
           "...with its IComponentHandler2 leg, not the IComponentHandler one" );
    {
        // Back from the second leg to the first: one object.
        Vst::IComponentHandler *back = nullptr;
        check( h2->queryInterface( Vst::IComponentHandler::iid, (void **)&back ) == kResultOk &&
                   back == h1,
               "IComponentHandler2 -> IComponentHandler gives the same handler" );
        if( back ) back->release();
    }
    audio::twVst3PlugInterfaceSupport support;
    check( support.isPlugInterfaceSupported( Vst::IComponentHandler2::iid ) == kResultTrue,
           "IPlugInterfaceSupport lists IComponentHandler2" );

    // 2. Reported, once.
    check( !handler.takeDirtyFlag(), "nothing is reported before any setDirty" );
    check( h2->setDirty( true ) == kResultOk, "setDirty(true) is accepted" );
    check( handler.takeDirtyFlag(), "setDirty(true) is reported" );
    check( !handler.takeDirtyFlag(), "...once: taking it clears it" );
    h2->setDirty( true );
    h2->setDirty( true );
    check( handler.takeDirtyFlag() && !handler.takeDirtyFlag(),
           "two marks between polls are one report" );

    // 3. setDirty(false) is the plugin saying it is clean, which only a save
    // decides: nothing to report.
    h2->setDirty( false );
    check( !handler.takeDirtyFlag(), "setDirty(false) reports nothing" );

    // 4. Inside a call the host made, on that thread: not news.
    {
        audio::twVst3HostCallScope scope( &handler );
        h2->setDirty( true );
    }
    check( !handler.takeDirtyFlag(),
           "setDirty(true) from inside a host-initiated call is not reported" );
    // ...while the same mark from ANOTHER thread during that call still is.
    {
        audio::twVst3HostCallScope scope( &handler );
        std::thread other( [h2] { h2->setDirty( true ); } );
        other.join();
    }
    check( handler.takeDirtyFlag(),
           "setDirty(true) from another thread during a host call is reported" );

    // 5. The rest of IComponentHandler2 answers honestly and changes nothing.
    check( h2->startGroupEdit() == kResultOk && h2->finishGroupEdit() == kResultOk,
           "startGroupEdit / finishGroupEdit are accepted" );
    check( h2->requestOpenEditor( "editor" ) == kNotImplemented,
           "requestOpenEditor says it is not implemented" );
    check( !handler.takeDirtyFlag() && !handler.takeEditFlag(),
           "...and none of them is reported as a change" );

    h2->release();

    std::cout << ( g_failures ? "FAILED" : "all passed" ) << std::endl;
    return g_failures == 0 ? 0 : 1;
}
