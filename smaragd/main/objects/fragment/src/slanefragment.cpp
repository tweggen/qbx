#include "app/objects/fragment/slanefragment.h"

#include "app/model/sappcontext.h"
#include "app/model/sautomationlane.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/persistence/sprojectloader.h"

#include "tw/graph/tw303aenv.h"
#include "tw/mix/twtrackmix.h"
#include "tw/events/tweventseq.h"
#include "tw/events/tweventsource.h"

#include <QDomElement>
#include <limits>

// An event edit is never bounded on the right — same reasoning, same
// constant, as STrack's EVENT_DIRTY_END (strack.cpp): the consumer of an
// event stream is class-1 (a synth voice carries state across pages), so a
// change at `a` can be heard at any later position (design 3.2, F9).
static constexpr offset_t FRAGMENT_EVENT_DIRTY_END =
    std::numeric_limits<offset_t>::max();

#include <cmath>

SLaneFragment::SLaneFragment( SProject *project )
    : SObject( project ),
      lastDuration_( 1 ),
      lastDurationValid_( false )
{
    tw303aEnvironment *env = SAppContext::get().get303aEnvironment();
    cpTrackMix_ = std::make_shared<twTrackMix>( *env );
    cpTrackMix_->init();
    // A fragment sums at UNITY (proposal 41 D1) — no fader, no plugin chain,
    // no rewire: the twTrackMix IS the root component. Width follows the
    // project's channel count (proposal 36 B4 discipline: a fragment's audio
    // children freeze pages of that width, exactly as a track's or the root
    // mixer's do).
    cpTrackMix_->setChannels( (idx_t)( project ? project->channels() : 2 ) );

    QObject::connect( this, SIGNAL( childObjectAdded( SLink & ) ),
                      this, SLOT( fragmentChildWasAdded( SLink & ) ) );
    QObject::connect( this, SIGNAL( childObjectRemoved( SLink & ) ),
                      this, SLOT( fragmentChildWasRemoved( SLink & ) ) );

    if( project ) {
        QObject::connect( project, SIGNAL( channelsChanged( int ) ),
                          this, SLOT( onProjectChannelsChanged( int ) ),
                          Qt::UniqueConnection );
    }
}

SLaneFragment::~SLaneFragment()
{
    cpTrackMix_.reset();
}

bool SLaneFragment::hasDuration() const
{
    return true;
}

// A fragment's duration is the end of its last child — the same rule
// SStdMixer uses (STrack's instrument-tail addition does not apply: a
// fragment has no instrument, D1). An empty fragment reports 1, not 0 (the
// universal container-extent sentinel, model/CONTRACT.md inv. 12), normalized
// to 0 at the project level exactly as every other container's is.
length_t SLaneFragment::getDuration() const
{
    if( !lastDurationValid_ ) {
        offset_t first, last = 0;
        int nUndefStart, nUndefDuration;
        getChildrenExtent( first, last, nUndefStart, nUndefDuration );
        lastDuration_ = ( last > 0 ) ? last : 1;
        lastDurationValid_ = true;
    }
    return lastDuration_;
}

std::shared_ptr<twComponent> SLaneFragment::getRootComponent()
{
    return cpTrackMix_;
}

QWidget *SLaneFragment::getDetailEditWidget( QWidget * )
{
    // No UI in M1 — Part B (the visual model) is proposal 41 M5+, and a
    // fragment is reached only through the SCut that windows it, which has
    // its own inline renderer.
    return nullptr;
}

QWidget *SLaneFragment::getInlineEditWidget( QWidget * )
{
    return nullptr;
}

SObjectRenderer *SLaneFragment::getInlineRenderer()
{
    return nullptr;
}

void SLaneFragment::bumpRenderChainEpoch()
{
    refreshClipGainCurves();
    if( cpTrackMix_ ) cpTrackMix_->bumpContentEpoch();
}

void SLaneFragment::bumpRenderChainEpochRange( offset_t start, offset_t end )
{
    refreshClipGainCurves();
    if( cpTrackMix_ ) cpTrackMix_->invalidatePagesInRange( start, end );
}

// Mirrors STrack::refreshClipGainCurves() (strack.cpp) exactly, over this
// fragment's own children instead of a track's — see the class header for
// why this exists at all (proposal 41 M2b: bumping the epoch alone stales
// PAGES, it does not re-pull a child's gain into the CLIP ENTRY twTrackMix
// applies gain through).
void SLaneFragment::refreshClipGainCurves()
{
    if( !cpTrackMix_ ) return;
    for( SLink *lk : childLinks() ) {
        if( !lk || !lk->hasStartTime() ) continue;
        SObject *obj = &lk->getSObject();
        if( obj->isLane() ) continue;           // never true for a fragment
                                                  // child, kept for parity
        if( obj->contentKind() == SContentKind::Event ) continue;  // M3
        if( SClipWindow *w = obj->windowTakeAt( -1 ) ) obj = &w->asObject();
        cpTrackMix_->setClipGainCurve(
            lk, obj->automationCurve( QStringLiteral( "cut:Gain" ) ) );
        cpTrackMix_->setClipGainScalar(
            lk, std::pow( 10.0, obj->getVolume() / 20.0 ) );
    }
}

void SLaneFragment::checkDurationChanged()
{
    const length_t oldDuration = lastDuration_;
    lastDurationValid_ = false;
    const length_t newDuration = getDuration();
    if( newDuration != oldDuration ) {
        emit durationChanged( newDuration );
    }
}

void SLaneFragment::onProjectChannelsChanged( int n )
{
    if( !cpTrackMix_ ) return;
    cpTrackMix_->setChannels( (idx_t) n );
    // Every page in this fragment's mix is now the wrong shape; §4.5's
    // width-mismatch-is-a-MISS rule keeps playback safe either way, but this
    // is what makes the change converge instead of waiting for an unrelated
    // edit to invalidate (mirrors STrack::setChannels' non-first-call path).
    invalidateRenderPath();
}

/**
 * A new clip link. Mirrors STrack::trackChildWasAdded: an Event-kind child
 * goes into our OWN eventClips_ (proposal 41 M3), an audio-kind child into
 * cpTrackMix_ — stripped of what a fragment does not have: no live-recording
 * short-circuit (a fragment cannot be armed), no nested-track mute/solo
 * forwarding (D8 — a fragment holds clips, not lanes).
 */
void SLaneFragment::fragmentChildWasAdded( SLink &child )
{
    if( !child.hasStartTime() ) return;
    if( !child.getSObject().hasDuration() ) return;

    QObject::connect( &child, SIGNAL( startTimeChanged( offset_t ) ),
                      this, SLOT( fragmentChildWasMoved( offset_t ) ) );
    QObject::connect( &( child.getSObject() ), SIGNAL( durationChanged( length_t ) ),
                      this, SLOT( fragmentChildDurationChanged( length_t ) ) );

    // Event-kind content goes into OUR OWN event clip set (proposal 41 M3),
    // never into the mix — its getRootComponent() is null, so a twTrackMix
    // entry over it would be a freeze that can never succeed. This mirrors
    // STrack::trackChildWasAdded's Event branch exactly (same key, same
    // resolveEventClip callback) so the window gating / note-off synthesis /
    // loop tiling is the SAME tested code, not a second implementation.
    if( child.getSObject().contentKind() == SContentKind::Event ) {
        SLink *key = &child;
        eventClips_.insertClip(
            key, child.getStartTime(), child.getSObject().getDurationBlocking(),
            [key]( offset_t clipPos ) {
                return key->getSObject().resolveEventClip( clipPos );
            } );
        lastDurationValid_ = false;
        checkDurationChanged();
        return;
    }

    const offset_t startTime = child.getStartTime();
    // BLOCKING read (proposal 19 discipline, see STrack::trackChildWasAdded):
    // a try-lock snapshot can race a revalidation worker that just scheduled
    // a Preview job on a freshly-created cut and return the pre-set default.
    const length_t duration = child.getSObject().getDurationBlocking();
    auto getComponentFn = [&child]() { return child.getRootComponent(); };
    // Inv-1 (proposal 19): one resolver, component + slip-folded position
    // from a single clip snapshot, so a freeze cannot straddle a lazy reader
    // build.
    auto resolveFn = [&child]( offset_t off ) {
        return child.getSObject().resolveClip( off );
    };
    twEditRange affected;
    if( cpTrackMix_ ) {
        twEditRange r = cpTrackMix_->insertClip(
            &child, startTime, duration, getComponentFn, resolveFn );
        affected.unite( r.start, r.end );
        ++audioChildCount_;
    }
    invalidateRenderPathRange( (offset_t) affected.start, (offset_t) affected.end );

    lastDurationValid_ = false;
    checkDurationChanged();
}

void SLaneFragment::fragmentChildWasRemoved( SLink &child )
{
    if( !child.hasStartTime() ) return;
    if( !child.getSObject().hasDuration() ) return;

    if( eventClips_.hasClip( &child ) ) {
        twFrameRange r = eventClips_.removeClip( &child );
        invalidateRenderPathRange( (offset_t) r.start, FRAGMENT_EVENT_DIRTY_END );
        lastDurationValid_ = false;
        checkDurationChanged();
        return;
    }

    twEditRange affected;
    if( cpTrackMix_ ) {
        twEditRange r = cpTrackMix_->removeClip( &child );
        affected.unite( r.start, r.end );
        if( audioChildCount_ > 0 ) --audioChildCount_;
    }
    invalidateRenderPathRange( (offset_t) affected.start, (offset_t) affected.end );

    lastDurationValid_ = false;
    checkDurationChanged();
}

void SLaneFragment::fragmentChildWasMoved( offset_t newTime )
{
    SLink *slink = (SLink *) (const SLink *) sender();
    if( !slink || !slink->getSObject().hasDuration() ) return;

    const length_t duration = slink->getSObject().getDurationBlocking();

    if( eventClips_.hasClip( slink ) ) {
        twFrameRange r = eventClips_.updateClip( slink, newTime, duration );
        invalidateRenderPathRange( (offset_t) r.start, FRAGMENT_EVENT_DIRTY_END );
        lastDurationValid_ = false;
        checkDurationChanged();
        return;
    }

    twEditRange affected;
    if( cpTrackMix_ ) {
        twEditRange r = cpTrackMix_->updateClip( slink, newTime, duration );
        affected.unite( r.start, r.end );
    }
    invalidateRenderPathRange( (offset_t) affected.start, (offset_t) affected.end );

    lastDurationValid_ = false;
    checkDurationChanged();
}

void SLaneFragment::fragmentChildDurationChanged( length_t newLength )
{
    SObject *obj = dynamic_cast<SObject *>( sender() );
    if( !obj ) return;

    if( obj->contentKind() == SContentKind::Event ) {
        for( SLink *lk : childLinks() ) {
            if( !lk || &lk->getSObject() != obj ) continue;
            if( !eventClips_.hasClip( lk ) ) continue;
            twFrameRange r = eventClips_.updateClip( lk, lk->getStartTime(), newLength );
            invalidateRenderPathRange( (offset_t) r.start, FRAGMENT_EVENT_DIRTY_END );
        }
    } else {
        twEditRange affected;
        for( SLink *lk : childLinks() ) {
            if( !lk || &lk->getSObject() != obj ) continue;
            if( cpTrackMix_ ) {
                twEditRange r = cpTrackMix_->updateClip(
                    lk, lk->getStartTime(), newLength );
                affected.unite( r.start, r.end );
            }
        }
        invalidateRenderPathRange( (offset_t) affected.start, (offset_t) affected.end );
    }

    lastDurationValid_ = false;
    checkDurationChanged();
}

// Proposal 41 D4/M3: flatten our own event clip set into ONE immutable
// snapshot, rebuilt fresh on every call (no dirty-flag cache — see the class
// comment: our children are write-once past M2's pack-clips, D3a, and this
// mirrors STrack::eventFeed()'s own "rebuilt on every read" discipline).
// clipPos is unused; the map is always identity because THIS object's own
// zero already IS the flattened table's zero — any slip/loop/channel remap
// belongs to the WINDOW (the SCut over us), never to the content underneath.
twEventClipResolved SLaneFragment::resolveEventFeed( offset_t )
{
    twEventClipResolved out;
    if( eventClips_.clipCount() == 0 ) return out;

    const length_t span = getDuration();
    if( span == 0 ) return out;

    twEventBlock block;
    eventClips_.collect( 0, (int64_t) span, block );
    if( block.events.empty() ) return out;

    out.seq = std::make_shared<const twEventSeq>(
        std::move( block.events ), std::move( block.arena ) );
    out.map = nullptr;   // identity — see the header comment
    return out;
}

SLink *SLaneFragment::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    // Not parented into `parent` — a fragment has no tree position of its
    // own (see the class comment / D2). It is document-level content, exactly
    // as SPluginChain::instantiateFromDomElement returns an unparented
    // handle; whatever SCut later windows it is what keeps it alive.
    (void) parent;

    SLaneFragment *fragment = new SLaneFragment( &projectLoader.getProject() );
    fragment->readPreChildrenAttributes( element );

    QDomNode childNode = element.firstChild();
    while( !childNode.isNull() ) {
        if( childNode.isElement() ) {
            QDomElement childElement = childNode.toElement();
            if( childElement.nodeName() == "SLink" ) {
                const QString objectId = childElement.attribute( "objectId" );
                SLink *contentLink = projectLoader.getObjectDictionary().value( objectId );
                if( contentLink ) {
                    SLink *sl = new SLink( contentLink->getSObject(), nullptr );
                    sl->readAttributes( childElement );
                    sl->setParent( fragment );
                } else {
                    qWarning() << "SLaneFragment: object not found in "
                                  "dictionary:" << objectId;
                }
            }
        }
        childNode = childNode.nextSibling();
    }
    fragment->readPostChildrenAttributes( element );

    return new SLink( *fragment, nullptr );
}

// Self-registration with the project loader (proposal 14 Phase 5 discipline,
// proposal 41 M1 AC1.5): the persistence module names no concrete types; each
// slice registers its own element name. Relies on the app being an OBJECT
// library (a STATIC lib would drop this TU's only reference, the static
// initializer, at link time — main/CMakeLists.txt's app_objects layer note).
//
// SElementKind::Container (matching STrack's and SStdMixer's registration,
// proposal 41 AC1.3): a child link this build cannot resolve — an unknown
// object kind, or a kind whose own element failed to instantiate — costs its
// own <SLink>, never the fragment. The fragment and every OTHER child it
// carries survive the round trip; only the unresolvable reference is dropped,
// with a warning naming it (SProjectLoader::pruneUnresolvable_).
static const bool s_registered_slanefragment = (
    SProjectLoader::registerSObjectClass( "SLaneFragment",
        SLaneFragment::instantiateFromDomElement,
        SElementKind::Container ), true );
