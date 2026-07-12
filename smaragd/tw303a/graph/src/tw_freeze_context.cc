#include "tw/graph/tw_freeze_context.h"
#include "tw/graph/twcomponent.h"

// Thread-local storage for the active freeze context
thread_local FreezeContext* FreezeContext::g_activeContext = nullptr;

FreezeContext::FreezeContext(twComponent& component)
    : component_(component), previousContext_(g_activeContext)
{
    // Install this context as the active freeze context for this thread
    g_activeContext = this;
}

FreezeContext::~FreezeContext()
{
    // Restore the previous context (or nullptr if this was the outermost context)
    g_activeContext = previousContext_;
}

FreezeContext* FreezeContext::current()
{
    return g_activeContext;
}

bool FreezeContext::isComponentInStack(const twComponent& comp)
{
    // Walk the entire FreezeContext stack to detect cycles through any component
    for (FreezeContext* ctx = g_activeContext; ctx != nullptr; ctx = ctx->previousContext_) {
        if (&ctx->getComponent() == &comp) {
            return true;  // Component is already being frozen somewhere in the stack
        }
    }
    return false;  // Component is not in the freeze stack
}
