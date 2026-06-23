#ifndef GENERATION_PROMISE_H
#define GENERATION_PROMISE_H

#include <cstdint>
#include <memory>
#include <future>
#include <mutex>
#include <unordered_map>
#include <chrono>

namespace audio {

/**
 * GenerationPromise: RAII-wrapped std::promise for revalidation completion.
 *
 * Each capture revalidation cycle gets a generation ID. Captures tagged with
 * that generation carry a future that resolves when all revalidators finish.
 *
 * This is the core of Phase 5c futures-based waiting for render completeness.
 *
 * Thread-safe: multiple threads can wait on the same promise.
 * RAII: promise destruction doesn't break futures (they hold shared_future).
 */
class GenerationPromise {
public:
    /**
     * Create a promise for the given generation ID.
     *
     * \param generation  Unique generation ID for this revalidation cycle
     */
    explicit GenerationPromise(uint32_t generation);

    ~GenerationPromise() = default;

    /**
     * Get the generation ID.
     */
    uint32_t getGeneration() const { return generation_; }

    /**
     * Mark this generation as complete (revalidation finished).
     *
     * Thread-safe; idempotent (calling multiple times is safe).
     * Wakes all waiters on getFuture().
     */
    void markComplete();

    /**
     * Get the future to wait on.
     *
     * Multiple threads can safely wait on the returned future.
     */
    std::shared_future<void> getFuture() const { return future_; }

    /**
     * Non-blocking check: is this generation ready?
     *
     * \param timeout  How long to wait (default 0ms = immediate check)
     * \return         True if generation is stable (future resolved)
     */
    bool isReady(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) const;

private:
    uint32_t generation_;
    std::promise<void> promise_;
    std::shared_future<void> future_;
};

/**
 * GenerationRegistry: Global registry of promises by generation ID.
 *
 * Singleton: getOrCreate() ensures one promise per generation.
 * Thread-safe: all access behind mutex.
 *
 * Used by:
 * - SCut/revalidator: create promise when invalidation cycle starts
 * - Revalidator workers: markComplete(gen) when all workers finish
 * - FileSink: wait on promise before writing generation to disk
 */
class GenerationRegistry {
public:
    /**
     * Get or create a promise for the given generation.
     *
     * If a promise already exists for this generation, return it.
     * Otherwise, create a new promise and add to registry.
     *
     * \param generation  Generation ID
     * \return            Shared promise for this generation
     */
    std::shared_ptr<GenerationPromise> getOrCreate(uint32_t generation);

    /**
     * Mark a generation as complete.
     *
     * If a promise exists for this generation, calls markComplete().
     * Idempotent: no-op if generation not in registry.
     *
     * \param generation  Generation ID to mark complete
     */
    void markComplete(uint32_t generation);

    /**
     * Check if a generation is ready (non-blocking).
     *
     * \param generation  Generation ID
     * \param timeout     How long to wait (default 0ms)
     * \return            True if generation is ready or not in registry
     */
    bool isReady(uint32_t generation,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) const;

    /**
     * Remove a generation from the registry (memory cleanup).
     *
     * Called after a generation is fully processed and written to disk.
     * Safe even if generation doesn't exist.
     *
     * \param generation  Generation ID to forget
     */
    void forget(uint32_t generation);

    /**
     * Get the singleton instance.
     */
    static GenerationRegistry& instance();

private:
    GenerationRegistry() = default;

    mutable std::mutex registryMutex_;
    std::unordered_map<uint32_t, std::shared_ptr<GenerationPromise>> promises_;

    // Deleted: no copying or moving the singleton
    GenerationRegistry(const GenerationRegistry&) = delete;
    GenerationRegistry& operator=(const GenerationRegistry&) = delete;
};

}  // namespace audio

#endif  // GENERATION_PROMISE_H
