#ifndef _TW_FREEZE_CONTEXT_H_
#define _TW_FREEZE_CONTEXT_H_

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
 * Proposal 19 dataflow stage 6 — the RT-thread freeze guard (assert-first
 * retirement). The realtime audio callback must NEVER render/freeze: it reads
 * ready pages and falls back to the stale predecessor (proposal 16). That has
 * been true structurally; this guard makes it ENFORCED: the RT callback marks
 * its thread once, and twComponent::freezePage refuses (one-shot stderr +
 * debug assert) if ever entered on it.
 */
class twRtThreadGuard {
public:
    static void markRtThread()   { isRt_ = true; }
    static bool onRtThread()     { return isRt_; }
private:
    inline static thread_local bool isRt_ = false;
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
