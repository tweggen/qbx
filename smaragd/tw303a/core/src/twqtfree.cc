#include "tw/core/twqtfree.h"

#include <atomic>
#include <mutex>

#include <QtCore/qglobal.h>

class QObject;

// Qt's hook table (qtcore/kernel/qhooks_p.h), exported from QtCore for tools
// such as GammaRay. Declared here rather than through Qt's private headers,
// which a distribution Qt need not ship. The index layout is Qt's own, stable
// since Qt 5.4: [0] version of the table, [1] its size, [3] the callback QObject's
// constructor calls (qt_addObject) once the object is set up.
extern quintptr Q_CORE_EXPORT qtHookData[];

namespace {

constexpr int kHookDataVersion = 0;
constexpr int kHookDataSize    = 1;
constexpr int kAddQObject      = 3;
using AddQObjectCallback = void ( * )( QObject * );

// A POD, deliberately: a thread_local with a non-trivial destructor corrupts
// the heap on the MinGW toolchain (tw/core/src/twlog.cc says how it was found).
thread_local int t_depth = 0;

std::atomic<std::uint64_t> g_violations{ 0 };
std::atomic<std::uint64_t> g_scopes{ 0 };
std::atomic<bool>          g_installed{ false };
AddQObjectCallback         g_previous = nullptr;
std::once_flag             g_once;

void onAddQObject( QObject *o )
{
    if( t_depth > 0 ) g_violations.fetch_add( 1, std::memory_order_relaxed );
    if( g_previous ) g_previous( o );
}

}  // namespace

twQtFreeScope::twQtFreeScope()
{
    ++t_depth;
    g_scopes.fetch_add( 1, std::memory_order_relaxed );
}

twQtFreeScope::~twQtFreeScope()
{
    --t_depth;
}

bool twQtFreeScope::active()
{
    return t_depth > 0;
}

namespace twQtFreeWatch {

bool install()
{
    std::call_once( g_once, [] {
        if( qtHookData[kHookDataVersion] < 1 || qtHookData[kHookDataSize] <= (quintptr)kAddQObject )
            return;
        g_previous = reinterpret_cast<AddQObjectCallback>( qtHookData[kAddQObject] );
        qtHookData[kAddQObject] = reinterpret_cast<quintptr>( &onAddQObject );
        g_installed.store( true, std::memory_order_release );
    } );
    return installed();
}

bool installed()
{
    return g_installed.load( std::memory_order_acquire );
}

void reset()
{
    g_violations.store( 0, std::memory_order_relaxed );
    g_scopes.store( 0, std::memory_order_relaxed );
}

std::uint64_t violations()
{
    return g_violations.load( std::memory_order_relaxed );
}

std::uint64_t scopesEntered()
{
    return g_scopes.load( std::memory_order_relaxed );
}

}  // namespace twQtFreeWatch
