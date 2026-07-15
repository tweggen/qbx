#ifndef _TW_VIEW_H
#define _TW_VIEW_H

#include "tw/graph/twcomponent.h"
#include <functional>
#include <memory>

class tw303aEnvironment;

/**
 * twView: Stable wrapper component that forwards to a dynamically-obtained component.
 *
 * Purpose: Bridge between twTrackMix (which needs stable component pointers for clips)
 * and SObject hierarchy (where component identity can change, e.g., SCut::getRootComponent).
 *
 * Design: twView holds a callback that returns the current component. This way:
 * - twTrackMix stores stable twView* pointers (no stale pointers)
 * - The callback dynamically gets the current component each time it's needed
 * - Decoupling preserved: tw303a doesn't depend on SObject/SLink
 *
 * Lifecycle: Created by STrack when a clip is added, destroyed when clip is removed.
 * The caller is responsible for keeping the underlying object alive as long as the
 * twView exists.
 */
class twView : public twComponent
{
public:
    // Callback signature: returns the current component to forward to
    using GetComponentFn = std::function<std::shared_ptr<twComponent>()>;
    // Callback signature: map a clip-relative position (what the track computes)
    // to the underlying component's position domain. A plain sample clip's
    // component is a reader over the SOURCE material, so the clip's slip offset
    // (SCut::startOffset, grain-stretched as needed) must be folded in before
    // seeking or freezing pages. Only the clip object knows that mapping; the
    // track and this wrapper stay domain-agnostic. Null = identity.
    using MapPosFn = std::function<offset_t(offset_t)>;

    twView(tw303aEnvironment &env, GetComponentFn getComponentFn,
           MapPosFn mapPosFn = nullptr);
    virtual ~twView();

    // Forward all rendering/seeking calls to the underlying component
    virtual int seekTo(offset_t offset) override;

    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo(IOVector& dest, idx_t outChannel) override;

    virtual std::shared_ptr<twOutputPage> freezePage(
        uint64_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

    virtual std::shared_ptr<twOutputPage> freezePreviewPage(
        uint64_t startPos,
        length_t length,
        int previewSampleRate,
        int fullSampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

    virtual idx_t getNInputs() const override;
    virtual idx_t getNOutputs() const override;
    virtual const char *getInputName(idx_t idx) const override;
    virtual const char *getOutputName(idx_t idx) const override;

    virtual void reset() override;
    virtual void createOutputLatches() override;

    // Teardown protocol
    virtual void teardown() override;

public:
    // Helper: safely get the underlying component with null check
    std::shared_ptr<twComponent> getComponent() const;

private:
    GetComponentFn getComponentFn_;
    MapPosFn mapPosFn_;

    // Apply mapPosFn_ (identity when unset). Call BEFORE getComponent(): the
    // mapping may lazily build the clip's reader chain, which changes what
    // getComponent() returns.
    offset_t mapPos(offset_t pos) const;
};

#endif
