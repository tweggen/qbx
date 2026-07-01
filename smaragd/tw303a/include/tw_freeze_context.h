#ifndef _TW_FREEZE_CONTEXT_H_
#define _TW_FREEZE_CONTEXT_H_

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
class FreezeContext {
public:
    // RAII guard: install this context as the active freeze context for this thread
    explicit FreezeContext(twComponent& component);

    // Automatically remove this context on destruction (restore previous context)
    ~FreezeContext();

    // Query the active freeze context for this thread (nullptr if not in freeze phase)
    static FreezeContext* current();

    // Get the component this context is marking as "being frozen"
    twComponent& getComponent() { return component_; }
    const twComponent& getComponent() const { return component_; }

private:
    twComponent& component_;
    FreezeContext* previousContext_;  // For restoring nested contexts

    // Thread-local active freeze context (nullptr if not in freeze phase)
    static thread_local FreezeContext* g_activeContext;
};

#endif
