#ifndef _SDRAINPENDINGDELETESACTION_H_
#define _SDRAINPENDINGDELETESACTION_H_

#include "app/actions/saction.h"

/**
 * `drain-pending-deletes` — collect the dead objects a loop-less caller would
 * otherwise leave pending: every posted event (the model's queued deletion
 * checks), then the `QEvent::DeferredDelete` events a `--test-case` run never
 * delivers.
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
 * WHAT IT FLUSHES (QBX-306): every posted event first, then DeferredDelete,
 * repeated until a round delivers nothing (bounded) -- SObject::
 * drainPendingDeletes(), the same call the shell's teardown makes. Since
 * QBX-306 removeRef() no longer calls deleteLater(): it posts a queued,
 * re-checking functor (Qt 6.11's deleteLater() is once-only, and a revalidator
 * worker may not call it at all), so the FIRST flush is what deletes a dead
 * SObject; the second collects widgets' deleteLater(). The runner's
 * per-action processEvents() delivers those functors too, so the paragraph
 * above now describes the history: a removed object is collected between
 * actions anyway, and this verb matters where no pump falls between the
 * removal and the claim (model CONTRACT invariant 8a).
 *
 * It is a TEST verb and not a fix: production pumps its own event loop. Use it
 * only where a case's claim is about an object's LIFETIME.
 *
 * `quiesce="1"` (QBX-315) first waits, bounded by `timeoutMs` (default 5000),
 * until the project's revalidator holds NO PIN: no reval job in flight and
 * none queued. A pinned SObject SWALLOWS its DeferredDelete (SObject::event)
 * and only the last unpin re-arms it, so without this the drain frees the
 * objects in one run and leaks them in the next, whichever way the worker
 * pool happened to be. With it the objects are freed every time, which is
 * what lets a gate that depends on their being GONE fail deterministically.
 * A timeout is a refusal, never a silent drain over a pinned graph.
 *
 * `collect="1"` (QBX-315, implies `quiesce`) then deletes every extern file
 * the project holds at REFCOUNT ZERO. That is the deletion the model already
 * asked for and Qt 6.11 dropped: a DeferredDelete that SObject::event()
 * swallowed (a resurrection, or a pin) sets Qt's once-only deleteLater flag,
 * so when the count reaches zero again the re-armed deleteLater() is a silent
 * no-op and the object leaks until teardown (QBX-306's measurement). Whether
 * an undone recording's SPlainWave leaks or dies depended on the ORDER the
 * drain met it and the cut holding it; this makes it die every time, which is
 * the state the live app reaches when the order goes the other way.
 */
class SDrainPendingDeletesAction : public SAction
{
public:
    SDrainPendingDeletesAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "drain-pending-deletes" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "quiesce" ), QStringLiteral( "collect" ),
             QStringLiteral( "timeoutMs" ) }; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    bool quiesce_   = false;
    bool collect_   = false;
    int  timeoutMs_ = 5000;
};

#endif // _SDRAINPENDINGDELETESACTION_H_
