#include "app/testkit/sdrainpendingdeletesaction.h"

#include <QCoreApplication>
#include <QDomElement>
#include <QEvent>

#include "app/actions/sactionregistry.h"
#include "app/model/sexternfile.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "tw/schedule/capture_revalidator.h"

#include <QElapsedTimer>
#include <QThread>

SApplyResult SDrainPendingDeletesAction::apply( SProject *project )
{
    if( quiesce_ || collect_ ) {
        // NO PIN MAY BE HELD when the deletes are delivered (QBX-315). pause()
        // returns once every in-flight reval job has exited -- and each exit
        // path unpins -- so a pause that finds the queue EMPTY means no job
        // holds a borrowed pointer. A job that re-queued itself on the way out
        // shows up as queued work, and the loop lets it run and asks again.
        // The last unpin hands its deletion check to the main thread before
        // pause() can return (QBX-306), so the drain below sees it.
        CaptureRevalidator *rv = project ? project->getRevalidator() : nullptr;
        if( rv ) {
            QElapsedTimer t;
            t.start();
            for( ;; ) {
                rv->pause();
                const size_t queued = rv->jobsQueued();
                rv->resume();
                if( queued == 0 ) break;
                if( t.elapsed() > timeoutMs_ ) {
                    qWarning( "drain-pending-deletes FAILED: the revalidator "
                              "still had %zu job(s) queued after %d ms",
                              queued, timeoutMs_ );
                    return { false, nullptr };
                }
                QCoreApplication::processEvents();
                QThread::msleep( 5 );
            }
        }
    }
    // The same drain smaragdOrderlyShutdown() runs before std::exit(), for the
    // same reason: nothing else in a --test-case run delivers these. It flushes
    // ALL posted events, then DeferredDelete, until nothing is left -- a
    // DeferredDelete-only flush misses the model's queued deletion checks
    // (SObject::removeRef, QBX-306; model CONTRACT invariant 8a).
    SObject::drainPendingDeletes();

    if( collect_ && project ) {
        // WHAT A WORKING deleteLater() WOULD HAVE FREED (see the header). No
        // pin is held -- the quiesce above saw to that -- and a refcount of
        // zero at an action boundary is garbage: nothing links the object, and
        // the model already asked for its deletion. Collected from a copy: the
        // destructor unregisters the file from the very hash being read.
        QList<SExternFile *> dead;
        const QHash<QString, SExternFile *> &xf = project->externFiles();
        for( auto it = xf.cbegin(); it != xf.cend(); ++it )
            if( it.value() && it.value()->refCount() == 0 )
                dead.append( it.value() );
        for( SExternFile *f : dead ) delete f;
        if( !dead.isEmpty() )
            qDebug( "drain-pending-deletes: collected %d extern file(s) at "
                    "refcount 0", (int) dead.size() );
    }
    return { true, nullptr };
}

void SDrainPendingDeletesAction::writeXml( QDomElement &elem ) const
{
    if( collect_ ) elem.setAttribute( "collect", 1 );
    if( quiesce_ ) {
        elem.setAttribute( "quiesce", 1 );
        elem.setAttribute( "timeoutMs", timeoutMs_ );
    }
}

bool SDrainPendingDeletesAction::readXml( const QDomElement &elem, int )
{
    quiesce_   = elem.attribute( "quiesce", "0" ).toInt() != 0;
    collect_   = elem.attribute( "collect", "0" ).toInt() != 0;
    timeoutMs_ = elem.attribute( "timeoutMs", "5000" ).toInt();
    return true;
}

static const bool s_reg_drain_pending_deletes = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "drain-pending-deletes" ),
        []{ return new SDrainPendingDeletesAction; } ), true );
