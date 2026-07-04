#include "twpluginchain.h"
#include <vector>
#include "twplugininsert.h"
#include <vector>
#include "io_vector.h"
#include <vector>
#include <algorithm>

twPluginChain::twPluginChain( tw303aEnvironment &env, idx_t nBusses )
    : twComponent( env ), nBusses_( nBusses )
{
    // allocPlugs() and createOutputLatches() are called by init()
}

twPluginChain::~twPluginChain() = default;

void twPluginChain::createOutputLatches()
{
    pOutputLatches_.resize(nBusses_);
    for( idx_t i = 0; i < nBusses_; ++i )
        pOutputLatches_[i] = std::make_shared<twStreamingLatch>( *this, i, 4096 );
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twPluginChain::calcOutputTo( IOVector& dest, idx_t port )
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    std::lock_guard<std::mutex> lock( pluginsMutex_ );

    if( plugins_.empty() ) {
        // No plugins: passthrough from input via IOVector
        if( port < (idx_t)pInputPlugs_.size() && pInputPlugs_[port] ) {
            std::vector<sample_t> buffer(dest.length());
            auto *input = static_cast<twLatchStreamingOutput *>(pInputPlugs_[port].get());
            length_t len = input->readStreamingData( buffer.data(), dest.length() );
            return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), len), 0, len);
        }
        return dest.fillSilence(0, dest.length());
    }

    // New block: reset all plugins so each processes fresh audio this callback.
    for( auto *plugin : plugins_ )
        plugin->resetBlock();

    // Pull from the last plugin's output (wiring already established in rebuildWiring)
    auto *lastPlugin = plugins_.back();
    if( lastPlugin && port < lastPlugin->getNOutputs() ) {
        return lastPlugin->calcOutputTo( dest, port );
    }

    return dest.fillSilence(0, dest.length());
}

void twPluginChain::addPlugin( audio::twPluginInsert *insert )
{
    if( insert ) {
        std::lock_guard<std::mutex> lock( pluginsMutex_ );
        plugins_.push_back( insert );
        rebuildWiring();
    }
}

void twPluginChain::removePlugin( int index )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    if( index >= 0 && index < (int)plugins_.size() ) {
        plugins_.erase( plugins_.begin() + index );
        rebuildWiring();
    }
}

void twPluginChain::reorderPlugin( int fromIndex, int toIndex )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    if( fromIndex < 0 || fromIndex >= (int)plugins_.size() ||
        toIndex < 0 || toIndex >= (int)plugins_.size() ||
        fromIndex == toIndex ) {
        return;
    }
    auto plugin = plugins_[fromIndex];
    plugins_.erase( plugins_.begin() + fromIndex );
    plugins_.insert( plugins_.begin() + toIndex, plugin );
    rebuildWiring();
}

int twPluginChain::seekTo( offset_t offset )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );

    // Forward seek to all plugins in the chain
    for( auto *plugin : plugins_ ) {
        if( plugin ) {
            plugin->seekTo( offset );
        }
    }

    // Also seek the input plugs (the source feeding into the chain)
    if( !pInputPlugs_.empty() ) {
        for( idx_t i = 0; i < nBusses_; ++i ) {
            if( i < (idx_t)pInputPlugs_.size() && pInputPlugs_[i] ) {
                twLatch &latch = pInputPlugs_[i]->getParentLatch();
                twComponent &comp = latch.getComponent();
                comp.seekTo( offset );
            }
        }
    }

    return 0;
}

void twPluginChain::rebuildWiring()
{
    // Set up wiring: input → plugin0 → plugin1 → ... → output
    // This is called once when plugins are added/removed, not on every sample block.

    if( plugins_.empty() ) {
        return;
    }

    // For each channel (port), set up the series wiring
    for( idx_t port = 0; port < nBusses_; ++port ) {
        // First plugin gets the track input
        auto *firstPlugin = plugins_[0];
        if( firstPlugin && port < firstPlugin->getNInputs() && port < (idx_t)pInputPlugs_.size() && pInputPlugs_[port] ) {
            firstPlugin->setInput( port, pInputPlugs_[port].get() );
        }

        // Wire each subsequent plugin to the previous one's output
        for( size_t i = 1; i < plugins_.size(); ++i ) {
            auto *prevPlugin = plugins_[i-1];
            auto *nextPlugin = plugins_[i];
            if( prevPlugin && nextPlugin &&
                port < prevPlugin->getNOutputs() && port < nextPlugin->getNInputs() ) {
                auto *prevOutput = prevPlugin->linkOutput( port );
                if( prevOutput ) {
                    nextPlugin->setInput( port, prevOutput );
                }
            }
        }
    }
}


void twPluginChain::reset()
{
    // Stateless component: plugins handle their own state
}

void twPluginChain::teardown()
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

    // Tear down each plugin in order (snapshot raw pointers, don't hold lock during teardown)
    std::vector<audio::twPluginInsert*> pluginsCopy;
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        pluginsCopy = plugins_;
        plugins_.clear();
    }

    for (auto plugin : pluginsCopy) {
        if (plugin) plugin->teardown();
    }
}

void twPluginChain::onDependencyTeardown(twComponent* dep)
{
    // A plugin is being torn down; remove it from our list
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    auto it = std::find_if(plugins_.begin(), plugins_.end(),
        [dep](audio::twPluginInsert *p) {
            return p == dep;
        });
    if (it != plugins_.end()) {
        plugins_.erase(it);
        rebuildWiring();
    }
}
