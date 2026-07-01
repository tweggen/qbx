#include "tw_freeze_context.h"
#include "twcomponent.h"

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

void FreezeContext::setInputPage(int inputIdx, std::shared_ptr<twOutputPage> page)
{
    inputPages_[inputIdx] = page;
}

std::shared_ptr<twOutputPage> FreezeContext::getInputPage(int inputIdx) const
{
    auto it = inputPages_.find(inputIdx);
    if (it != inputPages_.end()) {
        return it->second;
    }
    return nullptr;
}
