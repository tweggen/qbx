#ifndef _TW_FROZEN_INPUTS_H_
#define _TW_FROZEN_INPUTS_H_

#include "tw/core/twtypes.h"
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class twComponent;
struct twOutputPage;

/**
 * Proposal 19 dataflow, stage 1 — the leaf renderer's explicit input set.
 *
 * A dataflow node renders ONE page of ONE component purely from
 * already-frozen input pages (see "Phase 2 REVISED: demand-driven dataflow"
 * in plan/proposed/19_ASYNC_FREEZE_MODEL.md). This is the container the
 * planner/scheduler hands to twComponent::freezePageFromInputs(): a set of
 * ready pages keyed by (producer component, page start position).
 *
 * The set is consulted at the ONE place the legacy model pulls inputs
 * synchronously — twStreamingLatch::copyData() — via a thread-scoped
 * installation (twFrozenInputScope): a bound page is served directly, with
 * NO recursive freezePage() into the producer. Because the key carries the
 * producer, one flat set can satisfy an entire nested render on this thread
 * (a parent's renderFrames reading several inputs, each mapping to its own
 * producer's pages).
 *
 * Trust contract: a bound page needs validAspects != 0 and the exact
 * startPosition; its CONTENT EPOCH IS NOT RE-CHECKED here. Epoch validity is
 * the caller's job (the dataflow scheduler verifies inputs at publish time —
 * "verify-at-publish"), which is what lets a planned node render
 * deterministically from the exact pages it was planned against.
 *
 * Misses: when a scope is installed but a wanted (producer, pageStart) is not
 * bound, the miss is RECORDED and — in stage 1 — the legacy recursive pull
 * runs as before (behaviour unchanged). Later stages turn misses into
 * "dependency not ready: abort node, re-plan" instead of recursing.
 *
 * Threading: an instance is owned by ONE rendering thread for the duration of
 * one leaf render; it is not shared and needs no locking.
 */
struct twFrozenInputs {
    struct Entry {
        const twComponent            *producer;
        offset_t                      pageStart;
        std::shared_ptr<twOutputPage> page;
    };

    std::vector<Entry> entries;

    // Deps copyData wanted but did not find (stage >1 turns these into
    // re-plan triggers). Mutable: recorded through the const active scope.
    mutable std::vector<std::pair<const twComponent *, offset_t>> misses;

    void bind( const twComponent *producer,
               std::shared_ptr<twOutputPage> page );

    std::shared_ptr<twOutputPage> find( const twComponent *producer,
                                        offset_t pageStart ) const;

    void noteMiss( const twComponent *producer, offset_t pageStart ) const {
        misses.emplace_back( producer, pageStart );
    }
};

/**
 * RAII thread-scoped installation of a twFrozenInputs set. Nests like
 * FreezeContext: an inner scope shadows the outer for its lifetime and
 * restores it on destruction. The scope covers the ENTIRE nested render on
 * this thread (see twFrozenInputs doc).
 */
class twFrozenInputScope {
public:
    // `self` is the component the scope's NODE renders (stage 2): its own
    // freezePage must render, not look itself up as a dependency — the input
    // set describes its INPUTS. Children looked up under the scope that are
    // not bound get a miss recorded and (stage 2) render via the legacy path.
    explicit twFrozenInputScope( const twFrozenInputs *inputs,
                                 const twComponent *self = nullptr )
        : previous_( active_ ), previousSelf_( activeSelf_ )
    {
        active_ = inputs;
        activeSelf_ = self;
    }
    ~twFrozenInputScope()
    {
        active_ = previous_;
        activeSelf_ = previousSelf_;
    }

    twFrozenInputScope( const twFrozenInputScope & ) = delete;
    twFrozenInputScope &operator=( const twFrozenInputScope & ) = delete;

    // The set active on this thread, or null when no leaf render is bound
    // (every legacy synchronous freeze).
    static const twFrozenInputs *active() { return active_; }
    // The component the active scope is rendering (see ctor), or null.
    static const twComponent *self() { return activeSelf_; }

private:
    const twFrozenInputs *previous_;
    const twComponent    *previousSelf_;
    inline static thread_local const twFrozenInputs *active_ = nullptr;
    inline static thread_local const twComponent    *activeSelf_ = nullptr;
};

#endif
