#include "app/model/sappcontext.h"

#include <cassert>

SAppContext *SAppContext::instance_ = nullptr;

void SAppContext::setInstance( SAppContext *ctx )
{
    instance_ = ctx;
}

SAppContext &SAppContext::get()
{
    // Set by SApplication's constructor before any project exists; a null
    // here means a unit test touched app-runtime code without an application.
    assert( instance_ );
    return *instance_;
}
