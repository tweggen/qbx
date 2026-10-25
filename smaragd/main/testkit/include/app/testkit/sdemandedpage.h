#ifndef SDEMANDEDPAGE_H
#define SDEMANDEDPAGE_H

#include <memory>

#include <QString>

#include "tw/core/twtypes.h"

class CaptureRevalidator;
class twComponent;
struct twOutputPage;

/**
 * Reading a page you have DEMANDED — the one rule two assertion verbs got
 * wrong the same way (QBX-223).
 *
 * `assert-clip-channels` and `assert-track-channels` both declare a demand on
 * the project's CaptureRevalidator and then read the component's cache. Both
 * read it with `twComponent::getPageIfExists()`, which is the AUDIO THREAD's
 * accessor: a try-lock that returns nullptr whenever another thread holds the
 * cache lock ("not available right now; the audio thread will output silence
 * this frame and try again next frame"). A verb that has just WAITED for its
 * demand has no next frame, so that nullptr reads as "the page was never
 * frozen" — and `assert-clip-channels` said exactly that, in a message about
 * channel width, on a 4-vCPU CI runner with four ctest jobs:
 *
 *     assert-clip-channels: no frozen page at 0 for clip "1,0"
 *
 * while the same case's sibling assertion had passed 0.176 s earlier and the
 * one after it passed too. capture_revalidator.cc:692 states the rule for
 * itself in one line — "getPagesInRange(), NOT getPageIfExists(): the latter
 * is the audio thread's TRY-lock probe and answers 'not right now' under
 * contention".
 *
 * That the try-lock is the half which fired there is measured, not assumed.
 * The same process ended `[SCHED] graph stats: nodesExecuted=72 nodeRetries=0
 * missPages=0 selfStale=0`: `selfStale` counts exactly the verify-at-publish
 * re-stale below and it is zero, no bound set missed, and the only production
 * caller of `retireComponentNodes()` is the live monitor, which that case
 * never enters. The page was there, current and frozen; the reader could not
 * see it.
 *
 * THE SECOND HALF, and why one demand is not enough either. A completed demand
 * does not promise a CURRENT page, BY DESIGN, on two documented paths. Note the
 * shapes differ: the try-lock half above makes a present page look ABSENT,
 * while this half leaves a page that looks present and valid and is OUTDATED —
 * `invalidatePagesInRange()` bumps the component's epoch and marks pages stale
 * IN PLACE, it does not clear `validAspects`. A verb testing only
 * `validAspects` therefore measured the outdated page and reported a number,
 * which is quieter and worse than failing. Staleness is
 * `page->contentEpoch < comp->contentEpochNow()`, same-counter only:
 *
 *   * verify-at-publish SELF-STALENESS — a node whose own component's content
 *     epoch moved during the render publishes its page and IMMEDIATELY
 *     re-stales it, deliberately before completing the demand, "because
 *     completing wakes the demand's waiters, and a waiter that observed the
 *     page as current in that window would consume exactly the outdated audio
 *     this check exists to reject". The engine's own consumer re-demands: the
 *     epoch-scoped readahead supersession in AudioEngine runs the position
 *     again, which re-plans against the edited graph.
 *   * NOT PRODUCED — `retireComponentNodes()` drops a queued node, so the
 *     demand finishes having produced nothing (`GraphDemand::notProduced()`),
 *     which a consumer is told to treat like a miss.
 *
 * So the discipline for a NON-RT reader of a demanded page is: demand, wait,
 * read with the BLOCKING accessor, test the page's EPOCH and not just its
 * aspects, and if it still does not read current, demand again — bounded, and
 * then FAIL NAMING THE WAIT. A bounded re-demand
 * cannot mask a page that is never frozen: the loop ends, and the message says
 * how many demands it spent and what the page looked like each time.
 *
 * It must never degrade into a sleep-poll. There is no sleep here: each attempt
 * blocks on the demand itself, so a fast box spends one attempt and a starved
 * one spends the same number of demands, just slower.
 */
namespace sdemandedpage {

struct Options {
    // How many demands this read may spend before it gives up. The default
    // covers one re-stale window; a case may lower it to 1 to WATCH the
    // give-up message (mc_capture_clip_width does, with expectReject).
    int  maxDemands = 4;

    // TEST-ONLY FAULT INJECTORS (QBX-223). They exist because neither failure
    // reproduces on a 12-core dev box: the contended lock needs a thread that
    // really holds it (contention makes unavailability likely, not certain),
    // and the re-stale needs an edit landing inside one page render.
    int  contendCacheMs    = 0;      // hold the cache lock across the 1st read
    bool staleAfterDemand  = false;  // re-stale the page after the 1st demand
};

struct Outcome {
    std::shared_ptr<twOutputPage> page;   // usable, or null — see `why`
    int     demands     = 0;              // how many were spent
    int     notProduced = 0;              // summed over those demands
    QString why;                          // empty exactly when `page` is set
};

/**
 * Demand `pageStart` of `comp`, wait, and read it back. `verb` names the
 * caller in every log line. `reval` may be null (SMARAGD_REVAL_WORKERS=0),
 * in which case this only reads what is already cached.
 */
Outcome read( CaptureRevalidator *reval,
              const std::shared_ptr<twComponent> &comp,
              offset_t pageStart,
              const char *verb,
              const Options &opt = Options() );

}  // namespace sdemandedpage

#endif // SDEMANDEDPAGE_H
