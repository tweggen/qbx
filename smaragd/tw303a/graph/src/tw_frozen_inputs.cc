#include "tw/graph/tw_frozen_inputs.h"
#include "tw/pages/tw_output_page.h"

#include <cassert>

void twFrozenInputs::bind( const twComponent *producer,
                           offset_t pageStart,
                           std::shared_ptr<twOutputPage> page )
{
    if( !producer || !page ) return;
    // Key on the DEMANDED position, which is what find() is queried with. A
    // producer that answers a demand with a differently-positioned page (a
    // remapping view, a defused RT-guard page) would otherwise bind under a key
    // no consumer asks for — a silent miss, or worse, another position's audio.
    assert( page->startPosition == pageStart );
    entries.push_back( Entry{ producer, pageStart, std::move( page ) } );
}

std::shared_ptr<twOutputPage> twFrozenInputs::find( const twComponent *producer,
                                                    offset_t pageStart ) const
{
    // Linear scan: a node's input set is small (its inputs' pages for one page
    // span, possibly a bound subtree) — a map would cost more than it saves.
    for( const Entry &e : entries ) {
        if( e.producer == producer && e.pageStart == pageStart )
            return e.page;
    }
    return nullptr;
}
