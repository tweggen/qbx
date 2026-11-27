// THE MODEL'S DELETION PATH (QBX-306).
//
// SObject::removeRef() used to answer a refcount of zero with deleteLater().
// While a CaptureRevalidator job pinned the object, SObject::event() swallowed
// that DeferredDelete and the LAST unpin re-armed it with a second
// deleteLater() -- from the worker, a raw std::thread, where deleteLater()
// makes Qt adopt the thread (THREADING rule 1, the mechanism of QBX-305's crash
// at thread exit). And on Qt 6.11 the re-arm deleted nothing at all, because
// deleteLater() is once-only per object: a swallowed one is never re-armed, so
// a pinned or resurrected object LEAKED. Measured; option (b) of the ticket.
//
// Now removeRef() posts a queued functor that RE-CHECKS (nRefs_ == 0,
// revalPins_ == 0, deletePending_) at delivery and deletes the object itself;
// the revalidator's last unpin is handed to the main thread the same way, and
// a resurrection simply takes the same path again. Loop-less callers collect
// with SObject::drainPendingDeletes() -- the shell's teardown and the
// drain-pending-deletes verb, which this test calls as a verb.
//
// WHY A UNIT TEST. The suite never reaches the pinned path (QBX-305 measured
// zero violations over every .qxa case): it needs an object whose refcount
// reaches zero while a job pins it, a race no verb can schedule. And a .qxa
// cannot tell "the drain deleted it" from "the runner's per-action
// processEvents() did", because the latter now delivers the functor too.
//
// Claims and the sabotage each must bite:
//  - an ordinary drop to zero is collected by drain-pending-deletes and NOT by
//    a DeferredDelete-only flush (sabotage: deleteLater() back in removeRef;
//    narrowing the drain to DeferredDelete only);
//  - 1->0->1->0 is collected, and an object re-referenced before delivery is
//    not (sabotage: drop the re-check);
//  - a pinned object survives the drain, its worker unpin constructs no
//    QObject, and it is collected once unpinned (on main: it leaks).

#include "app/model/sobject.h"
#include "app/testkit/sdrainpendingdeletesaction.h"

#include "tw/core/twqtfree.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>

#include <cstdio>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void check( bool ok, const std::string &what )
{
    std::printf( "%s %s\n", ok ? "ok  " : "FAIL", what.c_str() );
    if( !ok ) ++g_failures;
}

// The smallest concrete SObject: no project, no component, no renderer.
class Leaf : public SObject
{
public:
    explicit Leaf( const char *name ) : SObject( nullptr )
    {
        setSName( QString::fromUtf8( name ) );
    }
    std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override { return nullptr; }
};

// The verb, exactly as a .qxa runs it.
void drainVerb()
{
    SDrainPendingDeletesAction verb;
    verb.apply( nullptr );
}

void deferredDeleteOnly()
{
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
}

// The worker's last unpin, as CaptureRevalidator::workerLoop runs it: a fresh
// raw std::thread inside a twQtFreeScope. Returns the violations it caused.
unsigned long long unpinOnWorker( SObject *o )
{
    twQtFreeWatch::reset();
    std::thread t( [o] {
        twQtFreeScope scope;
        o->revalRemoveRef();
    } );
    t.join();
    const auto v = twQtFreeWatch::violations();
    const auto s = twQtFreeWatch::scopesEntered();
    std::printf( "     worker unpin: %llu QObject(s) constructed inside %llu scope(s)\n",
                 (unsigned long long)v, (unsigned long long)s );
    check( s >= 1, "the worker's unpin ran inside a Qt-free scope" );
    return (unsigned long long)v;
}

// Keeps posting itself an event: a drain must stop at its bound.
class Reposter : public QObject
{
public:
    int seen = 0;
    bool event( QEvent *e ) override
    {
        if( e->type() == QEvent::User ) {
            ++seen;
            QCoreApplication::postEvent( this, new QEvent( QEvent::User ) );
            return true;
        }
        return QObject::event( e );
    }
};

}  // namespace

int main( int argc, char **argv )
{
    std::setvbuf( stdout, nullptr, _IONBF, 0 );   // a crash must not eat the log
    QApplication app( argc, argv );

    check( twQtFreeWatch::install(), "the QObject-construction hook is available" );

    // --- an ordinary drop to zero ---------------------------------------
    {
        Leaf *o = new Leaf( "plain" );
        QPointer<Leaf> alive( o );
        o->addRef();
        o->removeRef();
        deferredDeleteOnly();
        check( !alive.isNull(),
               "plain: a DeferredDelete-only flush does not delete it (no deleteLater)" );
        drainVerb();
        check( alive.isNull(), "plain: drain-pending-deletes deletes it" );
    }

    // --- resurrection --------------------------------------------------
    {
        Leaf *o = new Leaf( "resurrected" );
        QPointer<Leaf> alive( o );
        o->addRef();
        o->removeRef();                      // 1 -> 0: a check is queued
        o->addRef();                         // 0 -> 1 before it is delivered
        drainVerb();
        check( !alive.isNull(),
               "resurrection: re-referenced before delivery, it survives the drain" );
        o->removeRef();                      // 1 -> 0 again
        drainVerb();
        check( alive.isNull(), "resurrection: 1->0->1->0 is collected" );
    }

    // --- a revalidator pin across the drop to zero ------------------------
    {
        Leaf *o = new Leaf( "pinned" );
        QPointer<Leaf> alive( o );
        o->addRef();
        o->revalAddRef();                    // a job holds a borrowed pointer
        o->removeRef();
        drainVerb();
        check( !alive.isNull(), "pinned: a pinned object survives the drain" );

        const auto v = unpinOnWorker( o );
        check( v == 0, "pinned: the last unpin constructs no QObject on the worker" );
        deferredDeleteOnly();
        check( !alive.isNull(),
               "pinned: a DeferredDelete-only flush does not collect it" );
        const int rounds = SObject::drainPendingDeletes();
        std::printf( "     drainPendingDeletes: %d productive round(s)\n", rounds );
        check( alive.isNull(), "pinned: unpinned, it is collected by the drain" );
    }

    // --- a pin taken and released while referenced -----------------------
    {
        Leaf *o = new Leaf( "live" );
        QPointer<Leaf> alive( o );
        o->addRef();
        o->revalAddRef();
        unpinOnWorker( o );
        drainVerb();
        check( !alive.isNull(), "live: an unpin of a referenced object deletes nothing" );
        o->removeRef();
        drainVerb();
        check( alive.isNull(), "live: its later drop to zero is collected" );
    }

    // --- a never-referenced object is not dead ---------------------------
    {
        Leaf *o = new Leaf( "fresh" );
        QPointer<Leaf> alive( o );
        o->revalAddRef();
        unpinOnWorker( o );
        drainVerb();
        check( !alive.isNull(), "fresh: an object never referenced survives an unpin" );
        delete o;
    }

    // --- destroyed before the queued work is delivered -------------------
    {
        Leaf *a = new Leaf( "deleted-after-removeRef" );
        a->addRef();
        a->removeRef();                      // check queued on qApp, via QPointer
        delete a;                            // an owner deletes it explicitly
        Leaf *b = new Leaf( "deleted-while-handed-off" );
        b->addRef();
        b->revalAddRef();
        b->removeRef();
        unpinOnWorker( b );                  // hand-off queued on b itself
        delete b;                            // ~QObject drops it
        drainVerb();
        check( true, "explicit deletes before delivery: the drain touches neither" );
    }

    // --- the drain is bounded --------------------------------------------
    {
        Reposter r;
        QCoreApplication::postEvent( &r, new QEvent( QEvent::User ) );
        const int rounds = SObject::drainPendingDeletes( 5 );
        check( rounds == 5 && r.seen == 5,
               "a drain against an endless poster stops at its bound" );
        QCoreApplication::removePostedEvents( &r );
    }

    std::printf( "%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures );
    return g_failures ? 1 : 0;
}
