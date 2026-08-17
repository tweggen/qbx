#include "tw/pages/io_vector.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <sstream>

// Proposal 36 §4.6: IOVector is a view over ONE CHANNEL of its pages. B1b named
// that channel once as a constant so that the day a wide twTrackMix needed
// `IOVector` over channel c the change would be a parameter and not another
// sweep. B4 is that day: the constant became `channel_`, a member set at
// construction, and every read/write below goes through it. Making IOVector
// itself width-aware is still rejected (§4.6) — wide mixing is a LOOP over
// channels of the same page pair, and the §4.4 clamp for a narrower SOURCE is
// the caller's decision (twPageClampChannel), never a view's.

// ========== Constructors ==========

IOVector::IOVector(std::shared_ptr<twOutputPage> page,
                   offset_t startOffset,
                   length_t length,
                   idx_t channel)
    : startOffset_(startOffset),
      length_(length),
      channel_(channel < 0 ? 0 : channel)
{
    if (page) {
        pages_.push_back(page);
    }
}

IOVector::IOVector(std::vector<std::shared_ptr<twOutputPage>> pages,
                   offset_t startOffset,
                   length_t length)
    : pages_(pages),
      startOffset_(startOffset),
      length_(length)
{
}

IOVector IOVector::CreateForPageOutput(std::shared_ptr<twOutputPage> page,
                                       idx_t channel)
{
    if (!page) {
        throw std::runtime_error("IOVector::CreateForPageOutput: page is null");
    }
    // THE PAGE'S OWN FRAME BOUND, not the class constant (proposal 36 §7 trap
    // 21, fixed at B9). This hard-coded FRAME_CAPACITY was a latent heap
    // overrun: twComponent::calcOutputTo builds a MONO SCRATCH page of the
    // caller's `length` (resizeMonoScratch), wrapped it here, and the vector
    // then claimed 65536 frames of room in a buffer that had `length`. Any
    // sub-page pull therefore wrote past the scratch and memcpy'd back more
    // than the caller's buffer held. It stayed latent only because the freeze
    // path always asks for a whole page.
    //
    // This is a NO-OP for every page the engine freezes: twOutputPage's
    // constructor sets channelFrames_ = FRAME_CAPACITY and only
    // resizeMonoScratch ever lowers it. So the value is unchanged everywhere
    // except exactly where it was wrong.
    return IOVector(page, 0, (length_t) page->channelFrames(), channel);
}

IOVector IOVector::CreateFromBuffer(sample_t* buffer, length_t lengthFrames)
{
    // Bridge for intermediate temp buffers in component rendering (twMixer, twMoog, etc).
    // These components generate output into heap-allocated vectors, then wrap them here
    // for copyFrom/copyTo operations with destination IOVectors.
    //
    // LIMITATION: This creates a temporary page view; the wrapped buffer is not owned
    // or tracked by the page, so the caller must ensure the buffer stays alive for
    // the duration of IOVector operations.
    //
    // TODO: Refactor components to render directly into page-backed IOVectors
    // instead of creating intermediate temp buffers.

    if (!buffer && lengthFrames > 0) {
        throw std::runtime_error("IOVector::CreateFromBuffer: buffer is null but length > 0");
    }

    // Create a temporary single-page backing
    auto tempPage = std::make_shared<twOutputPage>();

    // Directly assign the buffer as the samples (without copying)
    // Note: This assumes the caller's vector stays alive during IOVector operations
    // Mono scratch: a plain buffer of the caller's length, no stride, width 1.
    tempPage->resizeMonoScratch((size_t)lengthFrames);
    std::copy(buffer, buffer + lengthFrames, tempPage->channelPtr(0));
    tempPage->validFrames = lengthFrames;
    tempPage->validAspects = twAspectAll;  // Mark as valid so readers can use it

    return IOVector(tempPage, 0, lengthFrames);
}

// ========== Validation ==========

bool IOVector::validate() const
{
    // Check all pages are valid
    for (const auto& page : pages_) {
        if (!page) {
            return false;
        }
    }

    // Check startOffset is within first page bounds
    if (!pages_.empty() && startOffset_ >= twOutputPage::FRAME_CAPACITY) {
        return false;
    }

    // Check length doesn't exceed available
    if (length_ > availableFrames()) {
        return false;
    }

    return true;
}

void IOVector::validateOrThrow(const char* context) const
{
    if (!validate()) {
        std::stringstream ss;
        ss << "IOVector validation failed at " << context << ": " << describe();
        throw std::runtime_error(ss.str());
    }
}

// ========== Accessors ==========

std::shared_ptr<twOutputPage> IOVector::pageAt(size_t index) const
{
    if (index >= pages_.size()) {
        throw std::out_of_range("IOVector::pageAt: index out of range");
    }
    return pages_[index];
}

length_t IOVector::availableFrames() const
{
    if (pages_.empty()) {
        return 0;
    }

    // Single page: available = (this page's frame bound - start offset).
    // Same trap-21 correction as CreateForPageOutput: clampLength() is what
    // stops a copy running off the end, and it cannot do that against a
    // constant when the page in hand is a shorter mono scratch buffer. Equal to
    // FRAME_CAPACITY for every frozen page.
    if (pages_.size() == 1) {
        const length_t bound = pages_[0]
            ? (length_t) pages_[0]->channelFrames()
            : (length_t) twOutputPage::FRAME_CAPACITY;
        return (startOffset_ >= bound) ? 0 : (bound - startOffset_);
    }

    // Multi-page: available = full first page - offset + middle pages + partial last page
    // For now, simplified: assume each page is full
    length_t available = (twOutputPage::FRAME_CAPACITY - startOffset_)
                       + (pages_.size() - 1) * twOutputPage::FRAME_CAPACITY;
    return available;
}

sample_t* IOVector::rawPointer() const
{
    if (pages_.size() != 1) {
        throw std::runtime_error("IOVector::rawPointer: multi-page buffer cannot be accessed as raw pointer");
    }
    if (!pages_[0]) {
        throw std::runtime_error("IOVector::rawPointer: page is null");
    }
    if (pages_[0]->channelFrames() == 0) {
        throw std::runtime_error("IOVector::rawPointer: page samples buffer is empty");
    }
    return pages_[0]->channelPtr(channel_) + startOffset_;
}

// ========== Helper Methods ==========

IOVector::LogicalToPhysical IOVector::mapOffset(offset_t logical) const
{
    if (pages_.empty()) {
        return {0, 0};
    }

    offset_t physicalOffset = startOffset_ + logical;
    size_t pageIndex = physicalOffset / twOutputPage::FRAME_CAPACITY;
    offset_t offsetInPage = physicalOffset % twOutputPage::FRAME_CAPACITY;

    if (pageIndex >= pages_.size()) {
        // Out of bounds
        return {pages_.size(), 0};
    }

    return {pageIndex, offsetInPage};
}

length_t IOVector::clampLength(offset_t logicalOffset, length_t requested) const
{
    length_t available = availableFrames();
    if (logicalOffset >= available) {
        return 0;
    }
    length_t remaining = available - logicalOffset;
    return (requested <= remaining) ? requested : remaining;
}

// ========== Copy Operations ==========

length_t IOVector::copyFrom(const IOVector& source,
                            offset_t srcOffset,
                            length_t numFrames)
{
    if (!validate() || !source.validate()) {
        return 0;
    }

    length_t actualFrames = clampLength(srcOffset, numFrames);
    if (actualFrames == 0) {
        return 0;
    }

    // For single-page case (common), use direct memcpy
    if (pages_.size() == 1 && source.pages_.size() == 1) {
        auto srcMap = source.mapOffset(srcOffset);
        auto dstMap = mapOffset(0);

        size_t srcByteOffset = srcMap.offsetInPage * sizeof(sample_t);
        size_t dstByteOffset = dstMap.offsetInPage * sizeof(sample_t);
        size_t copyBytes = actualFrames * sizeof(sample_t);

        if (pages_[0] && source.pages_[0] &&
            srcMap.pageIndex < source.pages_.size() &&
            dstMap.pageIndex < pages_.size()) {

            memcpy(pages_[dstMap.pageIndex]->channelPtr(channel_) + dstMap.offsetInPage,
                   source.pages_[srcMap.pageIndex]->channelPtr(source.channel_) + srcMap.offsetInPage,
                   copyBytes);
        }
    } else {
        // Multi-page: copy frame by frame (simple, correct; not optimized)
        for (offset_t i = 0; i < actualFrames; ++i) {
            auto srcMap = source.mapOffset(srcOffset + i);
            auto dstMap = mapOffset(i);

            if (srcMap.pageIndex < source.pages_.size() && dstMap.pageIndex < pages_.size() &&
                source.pages_[srcMap.pageIndex] && pages_[dstMap.pageIndex] &&
                srcMap.offsetInPage < (offset_t)source.pages_[srcMap.pageIndex]->channelFrames() &&
                dstMap.offsetInPage < (offset_t)pages_[dstMap.pageIndex]->channelFrames()) {

                pages_[dstMap.pageIndex]->channelPtr(channel_)[dstMap.offsetInPage] =
                    source.pages_[srcMap.pageIndex]->channelPtr(source.channel_)[srcMap.offsetInPage];
            }
        }
    }

    return actualFrames;
}

length_t IOVector::copyTo(IOVector& dest,
                         offset_t dstOffset,
                         length_t numFrames) const
{
    return dest.copyFrom(*this, 0, numFrames);
}

length_t IOVector::mixFrom(const IOVector& source,
                          offset_t dstOffset,
                          length_t numFrames)
{
    if (!validate() || !source.validate()) {
        return 0;
    }

    length_t actualFrames = clampLength(dstOffset, numFrames);
    if (actualFrames == 0) {
        return 0;
    }

    // For single-page case (common), use direct mix loop
    if (pages_.size() == 1 && source.pages_.size() == 1) {
        auto srcMap = source.mapOffset(0);
        auto dstMap = mapOffset(dstOffset);

        if (pages_[0] && source.pages_[0] &&
            srcMap.pageIndex < source.pages_.size() &&
            dstMap.pageIndex < pages_.size()) {

            sample_t* dstPtr = pages_[dstMap.pageIndex]->channelPtr(channel_) + dstMap.offsetInPage;
            const sample_t* srcPtr =
                source.pages_[srcMap.pageIndex]->channelPtr(source.channel_) + srcMap.offsetInPage;

            for (length_t i = 0; i < actualFrames; ++i) {
                dstPtr[i] += srcPtr[i];
            }
        }
    } else {
        // Multi-page: mix frame by frame
        for (offset_t i = 0; i < actualFrames; ++i) {
            auto srcMap = source.mapOffset(i);
            auto dstMap = mapOffset(dstOffset + i);

            if (srcMap.pageIndex < source.pages_.size() && dstMap.pageIndex < pages_.size() &&
                source.pages_[srcMap.pageIndex] && pages_[dstMap.pageIndex] &&
                srcMap.offsetInPage < (offset_t)source.pages_[srcMap.pageIndex]->channelFrames() &&
                dstMap.offsetInPage < (offset_t)pages_[dstMap.pageIndex]->channelFrames()) {

                pages_[dstMap.pageIndex]->channelPtr(channel_)[dstMap.offsetInPage] +=
                    source.pages_[srcMap.pageIndex]->channelPtr(source.channel_)[srcMap.offsetInPage];
            }
        }
    }

    return actualFrames;
}

length_t IOVector::fillSilence(offset_t dstOffset, length_t numFrames)
{
    if (!validate()) {
        return 0;
    }

    length_t actualFrames = clampLength(dstOffset, numFrames);
    if (actualFrames == 0) {
        return 0;
    }

    // For single-page case, use memset
    if (pages_.size() == 1) {
        auto dstMap = mapOffset(dstOffset);

        if (pages_[0] && dstMap.pageIndex < pages_.size()) {
            memset(pages_[dstMap.pageIndex]->channelPtr(channel_) + dstMap.offsetInPage,
                   0, actualFrames * sizeof(sample_t));
        }
    } else {
        // Multi-page: zero frame by frame
        for (offset_t i = 0; i < actualFrames; ++i) {
            auto dstMap = mapOffset(dstOffset + i);

            if (dstMap.pageIndex < pages_.size() && pages_[dstMap.pageIndex] &&
                dstMap.offsetInPage < (offset_t)pages_[dstMap.pageIndex]->channelFrames()) {

                pages_[dstMap.pageIndex]->channelPtr(channel_)[dstMap.offsetInPage] = 0.0f;
            }
        }
    }

    return actualFrames;
}

length_t IOVector::fillConstant(offset_t dstOffset, length_t numFrames, sample_t value)
{
    if (!validate()) {
        return 0;
    }

    length_t actualFrames = clampLength(dstOffset, numFrames);
    if (actualFrames == 0) {
        return 0;
    }

    // For single-page case, use direct loop (can't memset for non-zero values)
    if (pages_.size() == 1) {
        auto dstMap = mapOffset(dstOffset);

        if (pages_[0] && dstMap.pageIndex < pages_.size() &&
            dstMap.offsetInPage < (offset_t)pages_[dstMap.pageIndex]->channelFrames()) {
            sample_t* dstPtr = pages_[dstMap.pageIndex]->channelPtr(channel_);
            for (offset_t i = 0; i < actualFrames; ++i) {
                dstPtr[dstMap.offsetInPage + i] = value;
            }
        }
    } else {
        // Multi-page: fill frame by frame
        for (offset_t i = 0; i < actualFrames; ++i) {
            auto dstMap = mapOffset(dstOffset + i);

            if (dstMap.pageIndex < pages_.size() && pages_[dstMap.pageIndex] &&
                dstMap.offsetInPage < (offset_t)pages_[dstMap.pageIndex]->channelFrames()) {

                pages_[dstMap.pageIndex]->channelPtr(channel_)[dstMap.offsetInPage] = value;
            }
        }
    }

    return actualFrames;
}

IOVector IOVector::slice(offset_t offset, length_t length) const
{
    // Create a new IOVector view starting at offset within this vector
    offset_t newStartOffset = startOffset_ + offset;
    length_t newLength = clampLength(offset, length);

    // Find which page(s) the slice starts in
    auto map = mapOffset(offset);

    if (map.pageIndex >= pages_.size()) {
        // Out of bounds: return empty vector
        return IOVector(nullptr, 0, 0);
    }

    // For now, return a vector starting from the mapped page
    std::vector<std::shared_ptr<twOutputPage>> slicePages(
        pages_.begin() + map.pageIndex, pages_.end());

    IOVector out(slicePages, map.offsetInPage, newLength);
    out.channel_ = channel_;
    return out;
}

// ========== Debugging ==========

std::string IOVector::describe() const
{
    std::stringstream ss;
    ss << "IOVector("
       << pages_.size() << " page" << (pages_.size() != 1 ? "s" : "")
       << ", off=" << startOffset_
       << ", len=" << length_
       << ", avail=" << availableFrames()
       << ")";
    return ss.str();
}
