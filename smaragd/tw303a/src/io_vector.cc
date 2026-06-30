#include "io_vector.h"
#include <cstring>
#include <stdexcept>
#include <sstream>

// ========== Constructors ==========

IOVector::IOVector(std::shared_ptr<twOutputPage> page,
                   offset_t startOffset,
                   length_t length)
    : startOffset_(startOffset),
      length_(length)
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

IOVector IOVector::CreateForPageOutput(std::shared_ptr<twOutputPage> page)
{
    if (!page) {
        throw std::runtime_error("IOVector::CreateForPageOutput: page is null");
    }
    return IOVector(page, 0, twOutputPage::FRAME_CAPACITY);
}

IOVector IOVector::CreateFromBuffer(sample_t* buffer, length_t lengthFrames)
{
    // For legacy interop only: wrap raw buffer in temporary structure
    // This is NOT memory-safe for multi-page operations
    if (!buffer && lengthFrames > 0) {
        throw std::runtime_error("IOVector::CreateFromBuffer: buffer is null but length > 0");
    }

    // Create a temporary single-page backing
    // Note: This doesn't actually own the buffer; caller must ensure buffer stays alive
    auto tempPage = std::make_shared<twOutputPage>();
    tempPage->samples.clear();
    // We can't safely wrap a raw buffer, so we'll just create an empty page
    // The caller should really use page-backed constructors
    fprintf(stderr, "WARNING: IOVector::CreateFromBuffer is deprecated; use page-backed constructors\n");

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

    // Single page: available = (page size - start offset)
    if (pages_.size() == 1) {
        length_t available = twOutputPage::FRAME_CAPACITY - startOffset_;
        return available;
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
    if (pages_[0]->samples.empty()) {
        throw std::runtime_error("IOVector::rawPointer: page samples buffer is empty");
    }
    return pages_[0]->samples.data() + startOffset_;
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

            memcpy(pages_[dstMap.pageIndex]->samples.data() + dstMap.offsetInPage,
                   source.pages_[srcMap.pageIndex]->samples.data() + srcMap.offsetInPage,
                   copyBytes);
        }
    } else {
        // Multi-page: copy frame by frame (simple, correct; not optimized)
        for (offset_t i = 0; i < actualFrames; ++i) {
            auto srcMap = source.mapOffset(srcOffset + i);
            auto dstMap = mapOffset(i);

            if (srcMap.pageIndex < source.pages_.size() && dstMap.pageIndex < pages_.size() &&
                source.pages_[srcMap.pageIndex] && pages_[dstMap.pageIndex] &&
                srcMap.offsetInPage < source.pages_[srcMap.pageIndex]->samples.size() &&
                dstMap.offsetInPage < pages_[dstMap.pageIndex]->samples.size()) {

                pages_[dstMap.pageIndex]->samples[dstMap.offsetInPage] =
                    source.pages_[srcMap.pageIndex]->samples[srcMap.offsetInPage];
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

            sample_t* dstPtr = pages_[dstMap.pageIndex]->samples.data() + dstMap.offsetInPage;
            sample_t* srcPtr = source.pages_[srcMap.pageIndex]->samples.data() + srcMap.offsetInPage;

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
                srcMap.offsetInPage < source.pages_[srcMap.pageIndex]->samples.size() &&
                dstMap.offsetInPage < pages_[dstMap.pageIndex]->samples.size()) {

                pages_[dstMap.pageIndex]->samples[dstMap.offsetInPage] +=
                    source.pages_[srcMap.pageIndex]->samples[srcMap.offsetInPage];
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
            memset(pages_[dstMap.pageIndex]->samples.data() + dstMap.offsetInPage,
                   0, actualFrames * sizeof(sample_t));
        }
    } else {
        // Multi-page: zero frame by frame
        for (offset_t i = 0; i < actualFrames; ++i) {
            auto dstMap = mapOffset(dstOffset + i);

            if (dstMap.pageIndex < pages_.size() && pages_[dstMap.pageIndex] &&
                dstMap.offsetInPage < pages_[dstMap.pageIndex]->samples.size()) {

                pages_[dstMap.pageIndex]->samples[dstMap.offsetInPage] = 0.0f;
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
            dstMap.offsetInPage < pages_[dstMap.pageIndex]->samples.size()) {
            for (offset_t i = 0; i < actualFrames; ++i) {
                pages_[dstMap.pageIndex]->samples[dstMap.offsetInPage + i] = value;
            }
        }
    } else {
        // Multi-page: fill frame by frame
        for (offset_t i = 0; i < actualFrames; ++i) {
            auto dstMap = mapOffset(dstOffset + i);

            if (dstMap.pageIndex < pages_.size() && pages_[dstMap.pageIndex] &&
                dstMap.offsetInPage < pages_[dstMap.pageIndex]->samples.size()) {

                pages_[dstMap.pageIndex]->samples[dstMap.offsetInPage] = value;
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

    return IOVector(slicePages, map.offsetInPage, newLength);
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
