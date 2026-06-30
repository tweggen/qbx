#include "twview.h"
#include "tw303aenv.h"
#include "io_vector.h"

twView::twView(tw303aEnvironment &env, GetComponentFn getComponentFn)
    : twComponent(env),
      getComponentFn_(getComponentFn)
{
}

twView::~twView()
{
}

twComponent *twView::getComponent() const
{
    if (!getComponentFn_) {
        return nullptr;
    }
    twComponent *comp = getComponentFn_();
    if (!comp) {
        fprintf(stderr, "WARNING: twView::getComponent() returned nullptr\n");
    }
    return comp;
}

int twView::seekTo(offset_t offset)
{
    twComponent *comp = getComponent();
    if (!comp) return -1;
    return comp->seekTo(offset);
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twView::calcOutputTo(IOVector& dest, idx_t outChannel)
{
    twComponent *comp = getComponent();
    if (!comp) {
        return dest.fillSilence(0, dest.length());
    }
    return comp->calcOutputTo(dest, outChannel);
}

// Legacy: Raw-pointer interface
length_t twView::calcOutputTo(sample_t *outBuffer, length_t outLen, idx_t outChannel)
{
    twComponent *comp = getComponent();
    if (!comp) {
        memset(outBuffer, 0, outLen * sizeof(sample_t));
        return outLen;
    }
    return comp->calcOutputTo(outBuffer, outLen, outChannel);
}

std::shared_ptr<twOutputPage> twView::freezePage(
    uint64_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    twComponent *comp = getComponent();
    if (!comp) {
        auto page = std::make_shared<twOutputPage>();
        page->startPosition = startPos;
        page->validFrames = 0;
        return page;
    }
    return comp->freezePage(startPos, inputData, inputOffset, inputLength, sampleRate, previousPage);
}

std::shared_ptr<twOutputPage> twView::freezePreviewPage(
    uint64_t startPos,
    length_t length,
    int previewSampleRate,
    int fullSampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    twComponent *comp = getComponent();
    if (!comp) {
        auto page = std::make_shared<twOutputPage>();
        page->startPosition = startPos;
        page->validFrames = 0;
        return page;
    }
    return comp->freezePreviewPage(startPos, length, previewSampleRate, fullSampleRate, previousPage);
}

idx_t twView::getNInputs() const
{
    twComponent *comp = getComponent();
    if (!comp) return 0;
    return comp->getNInputs();
}

idx_t twView::getNOutputs() const
{
    twComponent *comp = getComponent();
    if (!comp) return 0;
    return comp->getNOutputs();
}

const char *twView::getInputName(idx_t idx) const
{
    twComponent *comp = getComponent();
    if (!comp) return nullptr;
    return comp->getInputName(idx);
}

const char *twView::getOutputName(idx_t idx) const
{
    twComponent *comp = getComponent();
    if (!comp) return nullptr;
    return comp->getOutputName(idx);
}

void twView::reset()
{
    twComponent *comp = getComponent();
    if (comp) comp->reset();
}

void twView::createOutputLatches()
{
    twComponent *comp = getComponent();
    if (comp) comp->createOutputLatches();
}
