#ifndef _TW_FREEZE_CONTEXT_H_
#define _TW_FREEZE_CONTEXT_H_

#include "tw/core/twlog.h"

#include <atomic>
#include <cstdint>
#include <memory>

// Forward declarations to avoid circular includes
class twComponent;

/**
 * FreezeContext is a cycle-detection RAII guard for page-based rendering.
 *
 * Purpose:
 *   During freezePage_nolock, a component is actively being rendered on this thread.
 *   If calcOutputTo or copyData tries to call freezePage on the same component again,
 *   that would be a cycle (e.g., A reads B reads A). FreezeContext detects this and
 *   signals "return silence" instead of recursing.
 *
 * Architecture:
 *   - RAII guard: installs/removes itself from thread-local storage on construction/destruction
 *   - Stack nesting: multiple contexts can nest safely (e.g., twRewire freezes while
 *     calling twTrackMix freezes); each stores the previous context and restores it
 *
 * Usage (in freezePage_nolock):
 *   {
 *       FreezeContext ctx(*this);  // marks this component as "being frozen"
 *       renderFrames(...);         // safe to call; FreezeContext prevents recursion
 *   } // ctx auto-removes itself on destruction
 *
 * Query (in copyData/readStreamingData):
 *   FreezeContext* ctx = FreezeContext::current();
 *   if (ctx && &ctx->getComponent() == &getComponent()) {
 *       // This component is already being frozen — cycle detected
 *       filled = 0;  // Return silence instead of recursing
 *   } else {
 *       // Safe to call freezePage
 *       auto page = getComponent().freezePage(...);
 *   }
 */
/**
 * The per-thread RENDER POLICY (proposal 19 stage 6, generalised by proposal
 * 21 L0 / design §4).
 *
 * Two kinds of thread must never render, for two different reasons and with
 * two different consequences:
 *
 *   - the REALTIME audio callback. It reads ready pages and falls back to the
 *     stale predecessor (proposal 16); a freeze there is a design violation
 *     that would block the callback on a mutex and a disk read. It stays a
 *     one-shot report plus a debug ASSERT — a bug to be fixed, not a condition
 *     to be tolerated.
 *
 *   - the LIVE GRAPH PUMP (proposal 21 L1a). It renders live-owned processors
 *     block by block and is allowed to touch a component's own mutex, but it
 *     may not enter the frozen-page machinery. Here a refusal is EXPECTED to
 *     happen occasionally in the wild — a preview or an asset capture can
 *     legitimately arrive at a component the pump owns — so it must be silent
 *     audio plus a COUNTER (`liveThreadRefusals`, which `assert-render-policy`
 *     bounds at exit) and one log line, never an assert that would take the
 *     process down for something the design says is recoverable.
 *
 * Hence one policy per thread and ONE check in twComponent::freezePage.
 * Both markers are POD thread_locals: a thread_local with a non-trivial
 * destructor corrupts the heap under MinGW at 3+ threads (see CLAUDE.md).
 */
enum class twRenderPolicy : unsigned char {
    Any,      // ordinary worker / main thread: rendering is its job
    Never,    // RT callback or live pump: reads only
};

class twRtThreadGuard {
public:
    // Marking the RT thread also marks it non-blocking for the log sink: a
    // record from here try_locks and counts a drop rather than waiting on a
    // worker (proposal 24 §2.2). Doing it HERE rather than at the speaker's
    // callback keeps the two markers from drifting apart — which matters
    // because this guard's own violation report is logged from this thread.
    static void markRtThread()
    {
        if( kind_ != Kind::Rt ) {
            kind_ = Kind::Rt;
            policy_ = twRenderPolicy::Never;
            ::tw::TwLog::markNonBlocking();
            ::tw::TwLog::nameThread( "audio-rt" );
        }
    }

    // The live pump's marker. Same non-blocking log discipline (it is a
    // realtime-priority thread in all but name), different consequence.
    static void markLiveThread()
    {
        if( kind_ != Kind::Live ) {
            kind_ = Kind::Live;
            policy_ = twRenderPolicy::Never;
            ::tw::TwLog::markNonBlocking();
            ::tw::TwLog::nameThread( "live-pump" );
        }
    }

    static twRenderPolicy policy() { return policy_; }
    static bool mayRender()        { return policy_ == twRenderPolicy::Any; }
    static bool onRtThread()       { return kind_ == Kind::Rt; }
    static bool onLiveThread()     { return kind_ == Kind::Live; }

    // Process-wide, for the exit assertion and the log. Counted on EVERY
    // refusal; logged ONCE, because a violating cascade would otherwise fill
    // the log with the same line at block rate.
    static std::uint64_t liveThreadRefusals()
    { return liveRefusals_.load( std::memory_order_relaxed ); }
    static std::uint64_t liveThreadReports()
    { return liveReports_.load( std::memory_order_relaxed ); }

    // Called by the ONE check in twComponent::freezePage.
    static void noteLiveRefusal( const void *component,
                                 unsigned long long startPos )
    {
        liveRefusals_.fetch_add( 1, std::memory_order_relaxed );
        if( !liveReported_.exchange( true ) ) {
            liveReports_.fetch_add( 1, std::memory_order_relaxed );
            TW_LOGW( "graph",
                     "freezePage() entered on the LIVE PUMP thread "
                     "(component=%p pos=%llu) — returning silence and counting. "
                     "The pump renders processors directly; the frozen-page "
                     "machinery is not reachable from it.",
                     component, startPos );
        }
    }

    // Tests only: the counters are process-wide by design.
    static void resetLiveCounters()
    {
        liveRefusals_.store( 0, std::memory_order_relaxed );
        liveReports_.store( 0, std::memory_order_relaxed );
        liveReported_.store( false, std::memory_order_relaxed );
    }

private:
    enum class Kind : unsigned char { None, Rt, Live };
    inline static thread_local Kind kind_ = Kind::None;
    inline static thread_local twRenderPolicy policy_ = twRenderPolicy::Any;

    inline static std::atomic<std::uint64_t> liveRefusals_{ 0 };
    inline static std::atomic<std::uint64_t> liveReports_{ 0 };
    inline static std::atomic<bool>          liveReported_{ false };
};

class FreezeContext {
public:
    // RAII guard: install this context as the active freeze context for this thread
    explicit FreezeContext(std::shared_ptr<twComponent> component);

    // Automatically remove this context on destruction (restore previous context)
    ~FreezeContext();

    // Query the active freeze context for this thread (nullptr if not in freeze phase)
    static FreezeContext* current();

    // Check if a component is already being frozen anywhere in the current stack
    // (detects cycles: if component is in the freeze stack, freezing it would recurse)
    static bool isComponentInStack(std::shared_ptr<twComponent> comp);

    // Same question, by raw identity. twComponent::seek() asks it to decide
    // whether a seek is EXTERNAL to every freeze on this thread or is the
    // component's own freeze repositioning itself; it cannot use the
    // shared_ptr overload, because shared_from_this() throws for a component
    // that is not shared-owned (several module tests hold theirs by value).
    static bool isComponentInStack(const twComponent *comp);

    // Get the component this context is marking as "being frozen"
    std::shared_ptr<twComponent> getComponent() { return component_; }
    const std::shared_ptr<twComponent> getComponent() const { return component_; }

private:
    std::shared_ptr<twComponent> component_;
    FreezeContext* previousContext_;  // For restoring nested contexts

    // Thread-local active freeze context (nullptr if not in freeze phase)
    static thread_local FreezeContext* g_activeContext;
};

#endif
