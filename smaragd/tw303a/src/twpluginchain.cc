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

    // With plugins, we need to thread audio through them sequentially.
    // For now, thread the first plugin's output (more complex chaining deferred).
    // TODO: implement proper series wiring of multiple plugins.
    if( pInputPlugs && pInputPlugs[port] && !plugins_.empty() ) {
        auto *firstPlugin = plugins_[0];
        // Wire the input to the first plugin's matching port
        if( port < firstPlugin->getNInputs() ) {
            firstPlugin->setInput( port, pInputPlugs[port] );
        }
        // Pull from the first plugin's output
        return firstPlugin->calcOutputTo( dst, len, port );
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
