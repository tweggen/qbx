#ifndef _TW_PAGE_PLAN_H_
#define _TW_PAGE_PLAN_H_

#include "tw/core/twtypes.h"
#include <cstdint>
#include <memory>
#include <vector>

class twComponent;

/**
 * Proposal 19 dataflow, stage 2 — the planner's output: the structural
 * snapshot of ONE dataflow node, `(component, pageStart, epoch)`, and its
 * input-page dependencies.
 *
 * Produced by twComponent::planPage(): a STRUCTURAL walk (no rendering) that
 * captures, under the component's own lock where needed, exactly which
 * producer pages this component's render of [pageStart, pageStart+capacity)
 * will consume — for latch consumers (mixer/rewire/plugin chains) the
 * grid-aligned producer pages of each input plug; for twTrackMix the
 * resolveClip()-resolved component and mapped position of every overlapping
 * clip (the Inv-1 single resolution, captured ONCE so plan and render agree).
 *
 * The scheduler (stage 3) freezes each dep, binds the resulting pages into a
 * twFrozenInputs set, and executes the node via freezePageWithInputs(); the
 * epoch captured here is its verify-at-publish reference. The deps hold
 * OWNING shared_ptrs — a planned node can never dangle (the retireObject
 * lesson).
 *
 * The predecessor-page dependency (pageStart - capacity of the SAME
 * component, for DSP state chaining) is implicit and scheduler-owned; it is
 * not listed here.
 */
struct twPageDep {
    std::shared_ptr<twComponent> producer;
    offset_t                     pageStart;
};

struct twPagePlan {
    std::shared_ptr<twComponent> component;
    offset_t                     pageStart = 0;
    uint64_t                     epoch     = 0;   // component epoch at plan time
    std::vector<twPageDep>       deps;
};

#endif
