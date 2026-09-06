#ifndef _SDRAINPENDINGDELETESACTION_H_
#define _SDRAINPENDINGDELETESACTION_H_

#include "app/actions/saction.h"

/**
 * `drain-pending-deletes` — deliver the `QEvent::DeferredDelete` events a
 * `--test-case` run otherwise never delivers.
 *
 * WHY IT EXISTS, and it is a narrow reason rather than a general convenience.
 * `SObject::removeRef()` answers a refcount reaching zero with `deleteLater()`,
 * which only takes effect when an event loop delivers the deferred delete. The
 * action runner pumps `QCoreApplication::processEvents()` after every action,
 * and that does NOT deliver DeferredDelete at loop level 0 — which is the same
 * fact `smaragdOrderlyShutdown()` already has to work around, draining them by
 * hand before `std::exit()`.
 *
 * The consequence for a gate: an object that SHOULD have died lingers for the
 * whole scripted run, so a case cannot tell "this is kept alive on purpose"
 * from "nothing has got round to deleting it". That made the send-lane undo
 * pin (proposal 45 M7 follow-up) untestable — removing the `addRef()` that
 * keeps a removed lane alive changed NOTHING in a headless run, while in the
 * real app, with a live event loop, the lane would be freed between the removal
 * and the undo. This verb closes that gap: call it between the two and the
 * unpinned lane is actually collected.
 *
 * It is a TEST verb and not a fix: production pumps its own event loop. Use it
 * only where a case's claim is about an object's LIFETIME.
 */
class SDrainPendingDeletesAction : public SAction
{
public:
    SDrainPendingDeletesAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "drain-pending-deletes" ); }
    QStringList knownAttributes() const override { return {}; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
};

#endif // _SDRAINPENDINGDELETESACTION_H_
