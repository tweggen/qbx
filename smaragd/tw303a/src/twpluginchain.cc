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
    if( pInputPlugs && pInputPlugs[port] ) {
        if( port < plugins_[0]->getNInputs() ) {
            plugins_[0]->setInput( port, pInputPlugs[port] );
        }

        // Wire each subsequent plugin to the previous one's output
        for( size_t i = 1; i < plugins_.size(); ++i ) {
            auto *prevPlugin = plugins_[i-1];
            auto *nextPlugin = plugins_[i];
            if( port < prevPlugin->getNOutputs() && port < nextPlugin->getNInputs() ) {
                nextPlugin->setInput( port, prevPlugin->linkOutput( port ) );
            }
        }

        // Pull from the last plugin's output
        auto *lastPlugin = plugins_.back();
        if( port < lastPlugin->getNOutputs() ) {
            return lastPlugin->calcOutputTo( dst, len, port );
        }
    }

    return 0;
}

void twPluginChain::addPlugin( audio::twPluginInsert *insert )
{
    if( insert ) {
        plugins_.push_back( insert );
        wiringDirty_ = true;
    }
}

void twPluginChain::removePlugin( int index )
{
    if( index >= 0 && index < (int)plugins_.size() ) {
        plugins_.erase( plugins_.begin() + index );
        wiringDirty_ = true;
    }
}

void twPluginChain::reorderPlugin( int fromIndex, int toIndex )
{
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
    // TODO: rebuild the plugin chain wiring when the order changes
    wiringDirty_ = false;
}
