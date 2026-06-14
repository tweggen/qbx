#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <vector>

#include "twsyslog.h"

#include "twcomponent.h"

#define DEBUG_COMPONENT

offset_t twComponent::tellPos() const
{
    return 0;
}

// --- Format negotiation defaults -----------------------------------------

twFormatCaps twComponent::getOutputCaps( idx_t /*idx*/ ) const
{
    // The engine exchanges mono Float32; `rates` left empty == any rate (the
    // negotiator intersects it with the candidate set D).
    twFormatCaps c;
    c.types        = { twSampleType::Float32 };
    c.channelCounts = { 1 };
    return c;
}

twFormatCaps twComponent::getInputCaps( idx_t /*idx*/ ) const
{
    twFormatCaps c;
    c.types        = { twSampleType::Float32 };
    c.channelCounts = { 1 };
    return c;
}

namespace {

std::vector<std::uint32_t> sortedUnique( std::vector<std::uint32_t> v )
{
    std::sort( v.begin(), v.end() );
    v.erase( std::unique( v.begin(), v.end() ), v.end() );
    return v;
}

std::vector<std::uint32_t> intersectRates( const std::vector<std::uint32_t> &a,
                                           const std::vector<std::uint32_t> &b )
{
    std::vector<std::uint32_t> out;
    std::set_intersection( a.begin(), a.end(), b.begin(), b.end(),
                           std::back_inserter( out ) );
    return out;
}

}  // namespace

bool twComponent::commitFormats( const twFormat *in,  idx_t nIn,
                                 const twFormat *out, idx_t nOut )
{
    committedIn_.assign( in, in + ( nIn > 0 ? nIn : 0 ) );
    for( idx_t j = 0; j < nOut; ++j ) {
        twFormat prev = ( (std::size_t) j < committedOut_.size() )
                      ? committedOut_[j] : twFormat{};
        if( prev != out[j] ) emit formatChanged( j, prev, out[j] );
    }
    committedOut_.assign( out, out + ( nOut > 0 ? nOut : 0 ) );
    return true;
}

bool twComponent::narrowCaps( twPortDomains &ports ) const
{
    // Default coupling: this node runs every port at one common rate (it
    // neither resamples nor rate-mixes). Intersect all port rate-domains and
    // write the result back to each. Monotone: the intersection is a subset of
    // every input domain, so it only ever removes candidates.
    std::vector<std::vector<std::uint32_t> *> r;
    for( auto &c : ports.in )  r.push_back( &c.rates );
    for( auto &c : ports.out ) r.push_back( &c.rates );
    if( r.empty() ) return false;

    std::vector<std::uint32_t> common = sortedUnique( *r[0] );
    for( std::size_t i = 1; i < r.size(); ++i )
        common = intersectRates( common, sortedUnique( *r[i] ) );

    bool changed = false;
    for( auto *pr : r ) {
        if( sortedUnique( *pr ) != common ) {
            *pr = common;
            changed = true;
        }
    }
    return changed;
}

bool twComponent::isSeekable() const
{
    return false;
}

int twComponent::seekTo( offset_t offset )
{
    fprintf(stderr, "[twComponent::seekTo] Base implementation called on %s with offset=%llu (seekTo not implemented for this component)\n",
            typeid(*this).name(), (unsigned long long)offset);
    fflush(stderr);
    return -1;
}

void twComponent::resetAllLatches()
{
    // Reset all output latches to offset 0, ensuring deterministic capture rebuilds.
    // This fixes the nil-operation bug where rebuilding without actual changes
    // would produce different audio due to persisted latch offsets.
    if( pOutputLatches ) {
        for( idx_t i = 0; i < getNOutputs(); i++ ) {
            if( pOutputLatches[i] ) {
                pOutputLatches[i]->resetOffset();
            }
        }
    }
}

void twComponent::allocPlugs()
{
    if( getNInputs()>0 ) {
        pInputPlugs = (twLatchOutput **) ::calloc (sizeof (twLatchOutput *), getNInputs() );
        if( !pInputPlugs ) {
            throw excStandard(
                "twComponent::twComponent(): "
                "Not enough memory to create input plug pointer table." );
        }
    } else {
        pInputPlugs = NULL;
    }
    if( getNOutputs()>0 ) {
        pOutputLatches = (twLatch **)  calloc (sizeof (twLatch *), getNOutputs() );
        if( !pOutputLatches ) {
            throw excStandard(
                "twComponent::twPlugs(): "
                "Not enough memory to create output plug pointer table." );
        }
    } else {
        pOutputLatches = NULL;
    }
}

void twComponent::init()
{
#ifdef DEBUG_COMPONENT
    syslog( LOG_DEBUG, "twComponent::init(): entered." );
#endif
    
    allocPlugs();
    createOutputLatches();
    
#ifdef DEBUG_COMPONENT
    syslog( LOG_DEBUG, "twComponent::init(): leaving." );
#endif
}

/**
 *	@method twComponent.linkOutput
 *		Connect to a components output.
 *	@synopsis twLatchOutput *twComponent::linkOutput(
 *      idx_t idx )
 *	@param </i><tt>idx_t</tt><i>idx
 *		The index of the output to connect to.
 *	@desc
 *		Every component has an arbitrary number of outputs.
 *		An arbitrary number of components may connect to this
 *		output by using the twLatchOutput returned by this function.
 *	@returns
 *		A pointer to the twLatchObject, which can be used to retrieve data.
 *		or <br>NULL, if an error occured.
 */
twLatchOutput * twComponent::linkOutput( idx_t idx )
{
    return pOutputLatches[idx]->addOutput();
}

void twComponent::setInput( idx_t idx, twLatchOutput *newOutput )
{
    if( pInputPlugs[idx] ) {
        pInputPlugs[idx]->getParentLatch().deleteOutput( pInputPlugs[idx] );
        pInputPlugs[idx] = NULL;
        if( !newOutput ) --inputsSet_;
    } else {
        if( newOutput ) ++inputsSet_;
    }
    pInputPlugs[idx] = newOutput;
}

/**
 * Use of this method is dangerous. However, it allows nice things.
 * take care of dangling references.
 */
twLatchOutput *twComponent::getInputPlug( idx_t idx ) const
{
    if( idx<0 || idx>getNInputs() ) return NULL;
    return pInputPlugs[idx];
} 

/**
 * Initialize everything needed for playback operation.
 * Subclasses should call their parent classes' implementation.
 */
int twComponent::doInitOperation( int /* initId */ ) 
{
    return 0;
}

int twComponent::initOperation( int initId )
{
    int oldId = currentOperation_;
    if( initId == oldId ) return 0;
    currentOperation_ = initId;
    int res = doInitOperation( initId );
    if( res<0 ) {
        currentOperation_ = oldId;
        return res;
    }
    return 0;
}

twComponent::~twComponent ()
{
    if( pInputPlugs ) free( pInputPlugs );
    if( pOutputLatches ) free( pOutputLatches );
}

/**
 *	@method twComponent.twComponent
 *		Constructor for twComponent object.
 *	@synopsis twComponent::twComponent()
 *	@desc
 *		Every part of the synthesis network is a component. In C++, all components
 *		are derived from the twComponent class.
 *		Each twComponent object has several inputs and several outputs.
 *		Every input is a pointer to a twLatchOutput object, every output is a
 *		pointer to a twLatch object.
 *		<br>Each component will allocate pointers for the input/output table,
 *		any derived class will have to initialize all of the objects,
 *		as we do not know, which kind of latches will be used.
 *	@returns
 *		(nothing, constructor)
 */
twComponent::twComponent( tw303aEnvironment &env0 )
    : currentOperation_( -1 ),
      inputsSet_(0),
      env( env0 ),
      pOutputLatches(0),
      pInputPlugs(0)
{    
}
