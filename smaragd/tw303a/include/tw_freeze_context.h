#ifndef _TW_FREEZE_CONTEXT_H_
#define _TW_FREEZE_CONTEXT_H_

#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>

// Forward declarations to avoid circular includes
class twComponent;
struct twOutputPage;

/**
 * FreezeContext manages the rendering state during component page freezing.
 *
 * Purpose:
 *   When freezing a component's output page (freezePage_nolock), all upstream
 *   inputs must be pre-frozen and available. This context holds those pre-frozen
 *   pages and provides a safe way for components to query them during rendering.
 *
 * Architecture:
 *   - RAII guard: automatically installs/removes itself from thread-local storage
 *   - Explicit: makes render phase clear to anyone reading the code
 *   - Type-safe: no magic boolean flags; uses proper C++ object semantics
 *   - Extensible: can hold additional rendering metadata (sample rates, flags, etc.)
 *
 * Usage (in freezePage_nolock):
 *   {
 *       FreezeContext ctx(*this);
 *
 *       // Pre-freeze all upstream inputs
 *       for (idx_t i = 0; i < getNInputs(); i++) {
 *           if (auto plug = getInputPlug(i)) {
 *               auto upstreamComp = &plug->getParentLatch().getComponent();
 *               auto page = upstreamComp->freezePage(...);
 *               ctx.setInputPage(i, page);
 *           }
 *       }
 *
 *       // Now renderFrames can safely access pre-frozen inputs
 *       renderFrames(output, length, nullptr, 0, 0);
 *   } // FreezeContext auto-removes itself on destruction
 *
 * Query (in calcOutputTo or readStreamingData):
 *   FreezeContext* ctx = FreezeContext::current();
 *   if (ctx) {
 *       // We're in a frozen render; use pre-frozen input pages
 *       auto page = ctx->getInputPage(idx);
 *       if (page) {
 *           // Copy from frozen page into output buffer
 *           memcpy(dest, page->samples.data(), ...);
 *           return;
 *       }
 *   }
 *   // Fallback to normal streaming for any missing inputs
 */
class FreezeContext {
public:
    // RAII guard: install this context as the active freeze context
    // Throws std::invalid_argument if component is nullptr
    explicit FreezeContext(twComponent& component);

    // Automatically remove this context on destruction (restore previous context)
    ~FreezeContext();

    // Query the active freeze context for this thread (nullptr if not in freeze phase)
    static FreezeContext* current();

    // Set a pre-frozen input page for the given input index
    // Called by freezePage_nolock after pre-freezing upstream components
    void setInputPage(int inputIdx, std::shared_ptr<twOutputPage> page);

    // Get pre-frozen input page (returns nullptr if not available or not set)
    // Called by calcOutputTo/readStreamingData to check for pre-frozen data
    std::shared_ptr<twOutputPage> getInputPage(int inputIdx) const;

    // Get the component this context is rendering
    twComponent& getComponent() { return component_; }
    const twComponent& getComponent() const { return component_; }

private:
    twComponent& component_;
    FreezeContext* previousContext_;  // For restoring nested contexts
    std::map<int, std::shared_ptr<twOutputPage>> inputPages_;

    // Thread-local active freeze context (nullptr if not in freeze phase)
    static thread_local FreezeContext* g_activeContext;
};

#endif
