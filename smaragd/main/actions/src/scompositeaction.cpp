#include "app/actions/scompositeaction.h"
#include <QDomDocument>

SCompositeAction::SCompositeAction( const QList<SAction *> &children )
    : children_( children )
{
}

SCompositeAction::~SCompositeAction()
{
    qDeleteAll( children_ );
}

SApplyResult SCompositeAction::apply( SProject *project )
{
    QList<SAction *> inverses;
    for( SAction *child : children_ ) {
        SApplyResult r = child->apply( project );
        if( !r.applied ) {
            // Roll back what we already did (reverse order, best effort).
            for( int i = inverses.size() - 1; i >= 0; --i ) {
                if( inverses[i] ) inverses[i]->apply( project );
            }
            qDeleteAll( inverses );
            return {false, nullptr};
        }
        inverses.append( r.inverse );   // may be null (non-undoable child)
    }

    // One null child inverse poisons the composite's undoability.
    bool undoable = true;
    for( SAction *inv : inverses ) {
        if( !inv ) { undoable = false; break; }
    }
    if( !undoable ) {
        qDeleteAll( inverses );
        return {true, nullptr};
    }

    QList<SAction *> reversed;
    reversed.reserve( inverses.size() );
    for( int i = inverses.size() - 1; i >= 0; --i ) {
        reversed.append( inverses[i] );
    }
    return {true, new SCompositeAction( reversed )};
}

void SCompositeAction::writeXml( QDomElement &elem ) const
{
    // Best-effort nesting for logs/diagnostics; composites are live-only.
    QDomDocument doc = elem.ownerDocument();
    for( SAction *child : children_ ) {
        QDomElement ce = doc.createElement( child->name() );
        child->writeXml( ce );
        elem.appendChild( ce );
    }
}

bool SCompositeAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return false;   // live-only
}
