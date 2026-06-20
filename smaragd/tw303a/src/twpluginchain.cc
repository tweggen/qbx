#include "twpluginchain.h"
#include "twplugininsert.h"
#include <algorithm>

twPluginChain::twPluginChain( tw303aEnvironment &env, idx_t nBusses )
    : twComponent( env ), nBusses_( nBusses )
{
    allocPlugs();
    createOutputLatches();
}

twPluginChain::~twPluginChain() = default;

void twPluginChain::createOutputLatches()
{
    pOutputLatches = new twLatch *[nBusses_];
    for( idx_t i = 0; i < nBusses_; ++i )
        pOutputLatches[i] = new twStreamingLatch( *this, i, 4096 );
}

length_t twPluginChain::calcOutputTo( sample_t *dst, length_t len, idx_t port )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );

    if( plugins_.empty() ) {
        // No plugins: passthrough from input
        if( pInputPlugs && pInputPlugs[port] ) {
            auto *input = static_cast<twLatchStreamingOutput *>(pInputPlugs[port]);
            return input->readStreamingData( dst, len );
        }
        return 0;
    }

    // Series-wire all plugins: input → plugin0 → plugin1 → ... → output
    // First plugin gets the track input
    if( pInputPlugs && pInputPlugs[port] && !plugins_.empty() ) {
        auto *firstPlugin = plugins_[0];
        if( firstPlugin && port < firstPlugin->getNInputs() ) {
            firstPlugin->setInput( port, pInputPlugs[port] );
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

        // Pull from the last plugin's output
        auto *lastPlugin = plugins_.back();
        if( lastPlugin && port < lastPlugin->getNOutputs() ) {
            return lastPlugin->calcOutputTo( dst, len, port );
        }
    }

    return 0;
}

void twPluginChain::addPlugin( audio::twPluginInsert *insert )
{
    if( insert ) {
        std::lock_guard<std::mutex> lock( pluginsMutex_ );
        plugins_.push_back( insert );
        wiringDirty_ = true;
    }
}

void twPluginChain::removePlugin( int index )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    if( index >= 0 && index < (int)plugins_.size() ) {
        plugins_.erase( plugins_.begin() + index );
        wiringDirty_ = true;
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
    wiringDirty_ = true;
}

void twPluginChain::rebuildWiring()
{
    // Set up wiring: input → plugin0 → plugin1 → ... → output
    // This is called once when plugins are added/removed, not on every sample block.

    if( plugins_.empty() ) {
        wiringDirty_ = false;
        return;
    }

    // For each channel (port), set up the series wiring
    for( idx_t port = 0; port < nBusses_; ++port ) {
        // First plugin gets the track input
        auto *firstPlugin = plugins_[0];
        if( firstPlugin && port < firstPlugin->getNInputs() && pInputPlugs && pInputPlugs[port] ) {
            firstPlugin->setInput( port, pInputPlugs[port] );
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

    wiringDirty_ = false;
}
