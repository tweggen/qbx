#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twplugin.h"
#include "tw/graph/tw303aenv.h"
#include "tw/pages/io_vector.h"
#include <cstring>
#include <algorithm>

namespace audio {

twPluginInsert::twPluginInsert( tw303aEnvironment &env, std::unique_ptr<twPlugin> plugin )
    : twComponent( env ), plugin_( std::move( plugin ) )
{
    const auto &io = plugin_->ioLayout();
    inScratch_.resize( io.audioInputs );
    outScratch_.resize( io.audioOutputs );

    for( auto &buf : inScratch_ )
        buf.resize( 4096 );
    for( auto &buf : outScratch_ )
        buf.resize( 4096 );

    // allocPlugs() and createOutputLatches() are called by init()
}

twPluginInsert::~twPluginInsert() = default;

// Caller must hold mutex(). Scratch buffers start at 4096 frames but freezePage
// is called with full pages (twOutputPage::FRAME_CAPACITY = 65536 frames);
// without growing them, the std::copy/process calls below overflow the heap.
void twPluginInsert::ensureScratchCapacity( length_t len )
{
	for( auto &buf : inScratch_ ) {
		if( buf.size() < (size_t)len ) buf.resize( len );
	}
	for( auto &buf : outScratch_ ) {
		if( buf.size() < (size_t)len ) buf.resize( len );
	}
}

void twPluginInsert::createOutputLatches()
{
    // Create output latches for each output port.
    idx_t nOut = getNOutputs();
    pOutputLatches_.resize(nOut);
    for( idx_t i = 0; i < nOut; ++i )
        pOutputLatches_[i] = std::make_shared<twStreamingLatch>( *this, i, 4096 );
}

idx_t twPluginInsert::getNInputs() const
{
    return plugin_->ioLayout().audioInputs;
}

idx_t twPluginInsert::getNOutputs() const
{
    return plugin_->ioLayout().audioOutputs;
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twPluginInsert::calcOutputTo( IOVector& dest, idx_t port )
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    std::lock_guard<std::mutex> lock(mutex());

    ensureScratchCapacity( dest.length() );

    if( !producedThisBlock_ ) {
        // Pull every input bus into de-interleaved scratch (one mono wire each).
        for( idx_t c = 0; c < getNInputs(); ++c )
            pullInput( c, inScratch_[c].data(), dest.length() );

        // Bypass = copy in->out; process plugin otherwise.
        if( bypass_ ) {
            copyChannels( inScratch_, outScratch_, getNInputs(), dest.length() );
        } else {
            // Prepare scratch buffers with input data pointers.
            std::vector<const float *> inPtrs( getNInputs() );
            std::vector<float *> outPtrs( getNOutputs() );
            for( idx_t c = 0; c < getNInputs(); ++c )
                inPtrs[c] = inScratch_[c].data();
            for( idx_t c = 0; c < getNOutputs(); ++c )
                outPtrs[c] = outScratch_[c].data();

            plugin_->process( inPtrs.data(), outPtrs.data(), dest.length() );
        }
        producedThisBlock_ = true;
    }

    // Write requested output port to IOVector destination
    if( port < getNOutputs() ) {
        return dest.copyFrom(IOVector::CreateFromBuffer(outScratch_[port].data(), dest.length()), 0, dest.length());
    }
    return 0;
}

length_t twPluginInsert::pullInput( idx_t port, sample_t *dst, length_t len )
{
    if( pInputPlugs_.empty() || port >= (idx_t)pInputPlugs_.size() || !pInputPlugs_[port] )
        return 0;

    auto *input = static_cast<twLatchStreamingOutput *>(pInputPlugs_[port].get());
    return input->readStreamingData( dst, len );
}

void twPluginInsert::copyChannels( const std::vector<std::vector<sample_t>> &in,
                                   std::vector<std::vector<sample_t>> &out,
                                   idx_t nChan, length_t len )
{
    for( idx_t c = 0; c < nChan && c < out.size(); ++c ) {
        std::copy( in[c].begin(), in[c].begin() + len, out[c].begin() );
    }
}

void twPluginInsert::setBypass( bool bypass )
{
    std::lock_guard<std::mutex> lock(mutex());
    setBypass_nolock(bypass);
}

// Caller must hold mutex()
void twPluginInsert::setBypass_nolock( bool bypass )
{
    bypass_ = bypass;
}

int twPluginInsert::seekTo( offset_t offset )
{
    // Forward seek to input plugs (the previous stage in the chain)
    if( !pInputPlugs_.empty() ) {
        for( idx_t i = 0; i < getNInputs(); ++i ) {
            if( i < (idx_t)pInputPlugs_.size() && pInputPlugs_[i] ) {
                twLatch &latch = pInputPlugs_[i]->getParentLatch();
                twComponent &comp = latch.getComponent();
                comp.seekTo( offset );
            }
        }
    }

    return 0;
}

void twPluginInsert::reset()
{
	std::lock_guard<std::mutex> lock(mutex());
	reset_nolock();
}

// Caller must hold mutex()
void twPluginInsert::reset_nolock()
{
	// Plugin insert propagates reset to its plugin
	producedThisBlock_ = false;
	if (plugin_) {
		plugin_->reset();
	}
}

std::shared_ptr<twOutputPage> twPluginInsert::freezePage(
    uint64_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage )
{
	// Phase 6c: Process plugin on frozen input pages (non-blocking)
	// Plugin state flows through sequential pages; reset if seeking

	if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
		// Return silence page for zombie component
		auto silencePage = std::make_shared<twOutputPage>();
		silencePage->setValidFrames(0);
		return silencePage;
	}

	std::lock_guard<std::mutex> lock(mutex());

	// Content epoch read before rendering: if an edit lands while we process,
	// the resulting page is already stale and consumers will re-request it.
	const uint64_t epochNow = contentEpochNow();

	ensureScratchCapacity( inputLength );

	// Cache page size on first call
	if (pageSize_ == 0) {
		pageSize_ = twOutputPage::FRAME_CAPACITY;
	}

	// Detect non-sequential pages (seek case)
	if (lastFrozenPos_ > 0 && startPos != lastFrozenPos_) {
		fprintf(stderr, "[twPluginInsert] Non-sequential page request at %llu (expected %llu); "
			"resetting plugin state\n", (unsigned long long)startPos, (unsigned long long)lastFrozenPos_);
		plugin_->reset();  // Reset to avoid state corruption
		producedThisBlock_ = false;
	}

	// Get frozen input pages from upstream
	for (idx_t c = 0; c < getNInputs(); ++c) {
		if (c >= (idx_t)pInputPlugs_.size() || !pInputPlugs_[c]) {
			std::fill(inScratch_[c].begin(), inScratch_[c].end(), 0.0f);
			continue;
		}

		auto *input = static_cast<twLatchStreamingOutput *>(pInputPlugs_[c].get());
		twLatch &latch = input->getParentLatch();
		twComponent &comp = latch.getComponent();

		// Freeze upstream to get input page
		auto inputPage = comp.freezePage(startPos, nullptr, 0, inputLength, sampleRate, previousPage);
		if (!inputPage) {
			std::fill(inScratch_[c].begin(), inScratch_[c].end(), 0.0f);
			continue;
		}

		// Copy frozen page samples into de-interleaved scratch buffer
		const float *pageData = static_cast<const float *>(inputPage->getDataPtr());
		uint32_t validFrames = inputPage->getValidFrames();
		size_t copyLen = std::min((size_t)inputLength, (size_t)validFrames);
		if (pageData && copyLen > 0) {
			std::copy(pageData, pageData + copyLen, inScratch_[c].begin());
		}
	}

	// Process plugin on de-interleaved buffers
	if (!bypass_) {
		// Prepare pointer arrays for plugin
		std::vector<const float *> inPtrs(getNInputs());
		std::vector<float *> outPtrs(getNOutputs());
		for (idx_t c = 0; c < getNInputs(); ++c) {
			inPtrs[c] = inScratch_[c].data();
		}
		for (idx_t c = 0; c < getNOutputs(); ++c) {
			outPtrs[c] = outScratch_[c].data();
		}

		// Process plugin (stateful; state carries to next page if sequential)
		plugin_->process(inPtrs.data(), outPtrs.data(), inputLength);
	} else {
		// Bypass: copy inputs to outputs
		copyChannels(inScratch_, outScratch_, getNInputs(), inputLength);
	}

	// Create frozen output page
	auto outputPage = std::make_shared<twOutputPage>();
	if (outputPage && !outputPage->samples.empty()) {
		// Interleave output channels into page (stereo only for now)
		if (getNOutputs() >= 2 && outScratch_.size() >= 2) {
			for (size_t i = 0; i < (size_t)inputLength && i < outputPage->samples.size() / 2; ++i) {
				outputPage->samples[i * 2] = outScratch_[0][i];      // L
				outputPage->samples[i * 2 + 1] = outScratch_[1][i];  // R
			}
		} else if (getNOutputs() >= 1 && !outScratch_.empty()) {
			// Mono: copy to both channels
			for (size_t i = 0; i < (size_t)inputLength && i < outputPage->samples.size() / 2; ++i) {
				float sample = outScratch_[0][i];
				outputPage->samples[i * 2] = sample;
				outputPage->samples[i * 2 + 1] = sample;
			}
		}
		outputPage->setStartPosition(startPos);
		outputPage->setValidFrames(inputLength);
		outputPage->contentEpoch.store(epochNow);
		outputPage->setValidAspects(twAspectPlayback);
	}

	// Update position tracker
	lastFrozenPos_ = startPos + inputLength;

	return outputPage;
}

void twPluginInsert::teardown()
{
	state_.store(ComponentState::ZOMBIE, std::memory_order_release);

	if (auto parent = parentComponent_.lock()) {
		if (myInputIndex_ >= 0) {
			parent->removeInput(myInputIndex_);
		}
	}

	std::vector<twComponent*> depsCopy;
	{
		std::lock_guard<std::mutex> lock(mutex());
		depsCopy = dependents_;
	}
	for (auto dep : depsCopy) {
		if (dep) dep->onDependencyTeardown(this);
	}

	// Plugin insert has no children, just mark ZOMBIE
}

}  // namespace audio
