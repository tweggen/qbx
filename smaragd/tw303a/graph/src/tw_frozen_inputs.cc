#include "tw/graph/tw_frozen_inputs.h"
#include "tw/pages/tw_output_page.h"

void twFrozenInputs::bind( const twComponent *producer,
                           std::shared_ptr<twOutputPage> page )
{
    if( !producer || !page ) return;
    entries.push_back( Entry{ producer, page->startPosition, std::move( page ) } );
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
