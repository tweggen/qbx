#ifndef _TW_VIEW_H
#define _TW_VIEW_H

#include "twcomponent.h"
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
    using GetComponentFn = std::function<twComponent*()>;

    twView(tw303aEnvironment &env, GetComponentFn getComponentFn);
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

private:
    GetComponentFn getComponentFn_;

    // Helper: safely get the underlying component with null check
    twComponent *getComponent() const;
};

#endif
