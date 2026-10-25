#include "app/testkit/sdemandedpage.h"

#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"
#include "tw/schedule/capture_revalidator.h"

#include <QDebug>

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

namespace {

const offset_t CAP = (offset_t) twOutputPage::FRAME_CAPACITY;

// The BLOCKING read. getPagesInRange() takes the component's state mutex and
// waits out the short critical section; getPageIfExists() try-locks it and
// answers "no page" instead, which is the whole defect (QBX-223).
std::shared_ptr<twOutputPage> readCached( const std::shared_ptr<twComponent> &comp,
                                          offset_t pageStart )
{
    for( const std::shared_ptr<twOutputPage> &p :
             comp->getPagesInRange( pageStart, pageStart + CAP ) ) {
        if( p && (offset_t) p->startPosition == pageStart ) return p;
    }
    return nullptr;
}

// Why this page cannot be measured. FOUR distinct states that one "no frozen
// page" message used to collapse into each other, and they call for different
// answers: absent means the plan produced nothing; aspect-invalid means a
// placeholder was allocated but never frozen; EPOCH-STALE means a page really
// was published and then re-staled, which is the state verify-at-publish
// leaves behind and the only one of the four a re-demand fixes; 0 frames means
// an empty page was published.
//
// THE EPOCH TEST IS THE ONE THAT WAS MISSING, and it is not a variant of the
// other two: invalidatePagesInRange() bumps the component's contentEpoch_ and
// marks pages stale IN PLACE — it does NOT clear validAspects (twcomponent.cc:
// "Intersecting pages ... keep their old stamp and read as stale against the
// new epoch"). So a verb that only tested validAspects would MEASURE an
// outdated page and report a number, which is quieter and worse than failing.
// Staleness is "page->contentEpoch < contentEpochNow()", same-counter only
// (schedule CONTRACT inv. 6).
QString whyUnusable( const std::shared_ptr<twComponent> &comp,
                     const std::shared_ptr<twOutputPage> &p )
{
    if( !p ) return QStringLiteral( "no page at that position" );
    if( p->validAspects.load() == 0 )
        return QStringLiteral( "page present but no aspect is valid "
                               "(a placeholder, never frozen)" );
    const uint64_t stamped = p->contentEpoch.load();
    const uint64_t current = comp->contentEpochNow();
    if( stamped < current )
        return QStringLiteral( "page present and valid but EPOCH-STALE "
                               "(stamped %1, component is at %2 - published, "
                               "then re-staled)" ).arg( stamped ).arg( current );
    if( p->validFrames == 0 )
        return QStringLiteral( "page present and valid but carrying 0 frames" );
    return QString();
}

// Read the cache while another thread HOLDS its mutex, so the reader's
// blocking-ness is measured rather than hoped for.
std::shared_ptr<twOutputPage> readUnderHeldLock( const std::shared_ptr<twComponent> &comp,
                                                 offset_t pageStart,
                                                 int holdMs,
                                                 const char *verb )
{
    std::promise<void> held;
    std::future<void>  heldFuture = held.get_future();

    std::thread holder( [&comp, holdMs, &held]() {
        twComponent::CacheLockForTests lock( *comp );
        held.set_value();
        std::this_thread::sleep_for( std::chrono::milliseconds( holdMs ) );
    } );
    heldFuture.wait();                    // the lock is now genuinely held

    qWarning() << verb << ": TEST INJECTOR holds the page-cache lock for"
               << holdMs << "ms (contendCacheMs, QBX-223)";

    const auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<twOutputPage> page = readCached( comp, pageStart );
    const auto waitedMs = (int) std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0 ).count();
    holder.join();

    // Two distinct lines, because a case has to be able to tell them apart:
    // only a BLOCKING reader waits out the holder, and only a try-lock one
    // comes back instantly — which on main returned nullptr and reported the
    // page as never frozen.
    if( waitedMs * 2 >= holdMs ) {
        qWarning() << verb << ": blocking page read waited" << waitedMs
                   << "ms for the page-cache lock (contendCacheMs, QBX-223)";
    } else {
        qWarning() << verb << ": page read did NOT wait for the contended "
                      "page-cache lock (" << waitedMs
                   << "ms) - a try-lock probe answers 'no page' here";
    }
    return page;
}

}  // namespace

namespace sdemandedpage {

Outcome read( CaptureRevalidator *reval,
              const std::shared_ptr<twComponent> &comp,
              offset_t pageStart,
              const char *verb,
              const Options &opt )
{
    Outcome out;
    if( !comp ) {
        out.why = QStringLiteral( "no component" );
        return out;
    }

    const int maxDemands = std::max( 1, opt.maxDemands );

    for( int attempt = 0; attempt < maxDemands; ++attempt ) {
        if( reval ) {
            auto demand = reval->requestGraphPages( comp, pageStart, 1 );
            if( demand ) {
                demand->wait();
                out.notProduced += demand->notProduced();
            }
        }
        ++out.demands;

        if( opt.staleAfterDemand && attempt == 0 ) {
            qWarning() << verb << ": TEST INJECTOR re-stales page"
                       << (qlonglong) pageStart
                       << "now that its demand has completed "
                          "(staleAfterDemand, QBX-223)";
            comp->invalidatePagesInRange( pageStart, pageStart + CAP );
        }

        const std::shared_ptr<twOutputPage> page =
            ( opt.contendCacheMs > 0 && attempt == 0 )
                ? readUnderHeldLock( comp, pageStart, opt.contendCacheMs, verb )
                : readCached( comp, pageStart );

        out.why = whyUnusable( comp, page );
        if( out.why.isEmpty() ) {
            if( attempt > 0 ) {
                qWarning() << verb << ": page" << (qlonglong) pageStart
                           << "did not read current after its first demand;"
                           << "a fresh demand produced it on attempt"
                           << ( attempt + 1 ) << "(QBX-223)";
            }
            out.page = page;
            return out;
        }
    }

    // Loudly, and naming the wait: a bounded re-demand must never be able to
    // read as "the page is not wide enough" or to pass quietly.
    qWarning() << verb << ": PAGE NOT READY at" << (qlonglong) pageStart
               << "after" << out.demands << "bounded demand(s) -" << out.why
               << "- notProduced" << out.notProduced;
    return out;
}

}  // namespace sdemandedpage
