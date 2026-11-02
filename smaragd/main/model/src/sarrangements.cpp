#include "app/model/sarrangements.h"

namespace sarrangements {

namespace {
ReachWalk &walkSlot()
{
    static ReachWalk w;
    return w;
}
}  // namespace

void setReachWalk( ReachWalk walk ) { walkSlot() = std::move( walk ); }

bool reaches( SObject *from, const SObject *to, QString *via, const PendingEdges *pending )
{
    if( via ) via->clear();
    if( !from || !to ) return false;
    if( walkSlot() ) return walkSlot()( from, to, pending, via );
    QSet<const SObject *> seen;
    return detail::reachesRec( from, to, seen, pending );
}

}  // namespace sarrangements
