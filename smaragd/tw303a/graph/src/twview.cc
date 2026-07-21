#include "tw/graph/twview.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"

twView::twView(tw303aEnvironment &env, GetComponentFn getComponentFn,
               ResolveFn resolveFn)
    : twComponent(env),
      getComponentFn_(getComponentFn),
      resolveFn_(resolveFn)
{
}

twResolvedClip twView::resolve(offset_t pos) const
{
    if (resolveFn_) {
        return resolveFn_(pos);
    }
    // No resolver: identity mapping over the component-only accessor (the old
    // null-MapPosFn behaviour).
    return twResolvedClip{ getComponent(), pos };
}

twView::~twView()
{
}

std::shared_ptr<twComponent> twView::getComponent() const
{
    if (!getComponentFn_) {
        return nullptr;
    }
    std::shared_ptr<twComponent> comp = getComponentFn_();
    if (!comp) {
        fprintf(stderr, "WARNING: twView::getComponent() returned nullptr\n");
    }
    return comp;
}

int twView::seekTo(offset_t offset)
{
    // Inv-1: resolve component + mapped position from ONE snapshot, so the
    // mapping is always folded against the very reader we seek.
    twResolvedClip r = resolve(offset);
    if (!r.component) return -1;
    return r.component->seekTo(r.mappedPos);
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twView::calcOutputTo(IOVector& dest, idx_t outChannel)
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    std::shared_ptr<twComponent> comp = getComponent();
    if (!comp) {
        return dest.fillSilence(0, dest.length());
    }
    return comp->calcOutputTo(dest, outChannel);
}

std::shared_ptr<twOutputPage> twView::freezePage(
    offset_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // Inv-1: resolve the component AND the clip-relative → component-domain
    // position mapping from ONE snapshot (slip offset folded in against the
    // reader we actually freeze). Pages end up cached on the component keyed by
    // its own (source) positions, so slipping a clip later still hits valid
    // pages.
    twResolvedClip r = resolve((offset_t) startPos);
    if (!r.component) {
        auto page = std::make_shared<twOutputPage>();
        page->startPosition = startPos;
        page->validFrames = 0;
        return page;
    }
    return r.component->freezePage((uint64_t) r.mappedPos, inputData, inputOffset, inputLength, sampleRate, previousPage);
}

std::shared_ptr<twOutputPage> twView::freezePreviewPage(
    offset_t startPos,
    length_t length,
    int previewSampleRate,
    int fullSampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    twResolvedClip r = resolve((offset_t) startPos);
    if (!r.component) {
        auto page = std::make_shared<twOutputPage>();
        page->startPosition = startPos;
        page->validFrames = 0;
        return page;
    }
    return r.component->freezePreviewPage((uint64_t) r.mappedPos, length, previewSampleRate, fullSampleRate, previousPage);
}

idx_t twView::getNInputs() const
{
    std::shared_ptr<twComponent> comp = getComponent();
    if (!comp) return 0;
    return comp->getNInputs();
}

idx_t twView::getNOutputs() const
{
    std::shared_ptr<twComponent> comp = getComponent();
    if (!comp) return 0;
    return comp->getNOutputs();
}

const char *twView::getInputName(idx_t idx) const
{
    std::shared_ptr<twComponent> comp = getComponent();
    if (!comp) return nullptr;
    return comp->getInputName(idx);
}

const char *twView::getOutputName(idx_t idx) const
{
    std::shared_ptr<twComponent> comp = getComponent();
    if (!comp) return nullptr;
    return comp->getOutputName(idx);
}

void twView::reset()
{
    std::shared_ptr<twComponent> comp = getComponent();
    if (comp) comp->reset();
}

void twView::createOutputLatches()
{
    std::shared_ptr<twComponent> comp = getComponent();
    if (comp) comp->createOutputLatches();
}

void twView::teardown()
{
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);

    if (auto parent = parentComponent_.lock()) {
        if (myInputIndex_ >= 0) {
            parent->removeInput(myInputIndex_);
        }
    }

    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(shared_from_this());
    }

    // Forward teardown to underlying component
    std::shared_ptr<twComponent> comp = getComponent();
    if (comp) {
        comp->teardown();
    }
}
