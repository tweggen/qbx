#include "app/actions/scompositeaction.h"
#include <QDomDocument>

#include "tw/core/twlog.h"

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
        // A CHILD OF A COMPOSITE ACTS IN THE COMPOSITE'S ROOT unless it names
        // one of its own (QBX-245, proposal 09 D21). Every edit-group fan-out
        // in the tree builds its children with a bare index path -- the paths
        // came out of seditgroups::expandClipPaths, which are relative to the
        // root the composite is about -- and a child left rootless then
        // resolved them against the MASTER. In an arrangement that is the
        // silent wrong-tree edit D21 exists to prevent: index {0} usually
        // exists in both trees, so the edit SUCCEEDS on the wrong clip.
        //
        // Done HERE, in the funnel, for applyPropagatingRoot's own stated
        // reason: there are twelve fan-out sites, each constructing children
        // at a different place, and a funnel cannot be forgotten.
        //
        // THE LIMIT, stated because it cannot be expressed: an empty root
        // means "the master" AND "unset", so a child that deliberately means
        // the master inside an arrangement-rooted composite cannot say so.
        // No such case exists today -- the two verbs whose paths are
        // master-rooted by definition (extract-arrangement, dissolve-
        // arrangement) use neither a composite nor a pathRoot -- and a future
        // one would need a third state rather than a widening of this.
        if( child && child->pathRoot().isEmpty() && !pathRoot_.isEmpty() )
            child->setPathRoot( pathRoot_ );
        SApplyResult r = applyPropagatingRoot( child, project );
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

    // One null child inverse poisons the composite's undoability -- and that
    // has to be ANNOUNCED (QBX-146). It was silent, which made a multi-clip
    // edit containing one non-undoable child look like an ordinary undoable
    // step while nothing went on the undo stack at all.
    int firstNull = -1;
    for( int i = 0; i < inverses.size(); ++i ) {
        if( !inverses[i] ) { firstNull = i; break; }
    }
    if( firstNull >= 0 ) {
        TW_LOGW( "model",
                 "composite: %d child action(s), but '%s' (child %d) produced "
                 "no inverse, so the WHOLE step is not undoable. QBX-146.",
                 children_.size(),
                 firstNull < children_.size()
                     ? children_[firstNull]->name().toUtf8().constData()
                     : "?",
                 firstNull );

        // NOT rolled back, deliberately, and this is the honest limit of the
        // conservative fix. By here every child has already applied, and the
        // one child that has no inverse is exactly the one that cannot be
        // rolled back -- so undoing the others would leave a PARTIAL state,
        // worse than either keeping or discarding the whole edit. Preventing
        // the situation is the real fix: an action that mutates must refuse
        // rather than succeed without an inverse (see SRemoveSampleAction),
        // which turns this into the child's `!r.applied` path above, where a
        // complete rollback IS possible.
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
