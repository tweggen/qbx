#include "tw/core/generation_promise.h"

namespace audio {

GenerationPromise::GenerationPromise(uint32_t generation)
    : generation_(generation)
{
    future_ = promise_.get_future().share();
}

void GenerationPromise::markComplete() {
    try {
        promise_.set_value();
    } catch (const std::future_error&) {
        // Already set (idempotent)
    }
}

bool GenerationPromise::isReady(std::chrono::milliseconds timeout) const {
    return future_.wait_for(timeout) == std::future_status::ready;
}

GenerationRegistry& GenerationRegistry::instance() {
    static GenerationRegistry reg;
    return reg;
}

std::shared_ptr<GenerationPromise> GenerationRegistry::getOrCreate(uint32_t generation) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    auto it = promises_.find(generation);
    if (it != promises_.end()) {
        return it->second;
    }

    auto promise = std::make_shared<GenerationPromise>(generation);
    promises_[generation] = promise;
    return promise;
}

void GenerationRegistry::markComplete(uint32_t generation) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    auto it = promises_.find(generation);
    if (it != promises_.end()) {
        it->second->markComplete();
    }
}

bool GenerationRegistry::isReady(uint32_t generation,
                                 std::chrono::milliseconds timeout) const {
    std::lock_guard<std::mutex> lock(registryMutex_);

    auto it = promises_.find(generation);
    if (it == promises_.end()) {
        return true;  // Not in registry = not tracking this generation
    }

    return it->second->isReady(timeout);
}

void GenerationRegistry::forget(uint32_t generation) {
    std::lock_guard<std::mutex> lock(registryMutex_);
    promises_.erase(generation);
}

}  // namespace audio
