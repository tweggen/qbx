
#ifndef _SOBJECT_H
#define _SOBJECT_H

#include <qobject.h>
#include "tw/graph/tw303aenv.h"
#include <qtextstream.h>
#include <QDomElement>
#include <QList>
#include <QSet>
#include <atomic>
#include <mutex>
#include <memory>
#include "app/model/sautomationlane.h"
#include "tw/events/tweventclipset.h"
#include "tw/pages/capture_page_pool.h"
#include "tw/schedule/revalidatable.h"

class QWidget;

class SClipWindow;
class twComponent;
class twRandomSource;
class SProject;
class SLink;
class SObjectRenderer;
class CaptureRevalidator;


/**
 * A lightweight, storage-agnostic view over a container's ordered SLink
 * children. Consumers iterate with a range-for
 * (`for( SLink *lk : obj->childLinks() )`) or index with at()/size(), and stay
 * decoupled from how the order is actually stored (an explicit list today, the
 * QObject child list historically). Order is owned by the container; QObject
 * parentage is only about lifetime.
 */
class SChildLinks {
public:
    using const_iterator = QList<SLink*>::const_iterator;
    explicit SChildLinks( const QList<SLink*> &list ) : list_( list ) {}
    const_iterator begin() const { return list_.cbegin(); }
    const_iterator end() const { return list_.cend(); }
    int size() const { return list_.size(); }
    bool isEmpty() const { return list_.isEmpty(); }
    SLink *at( int i ) const { return list_.at( i ); }
private:
    const QList<SLink*> &list_;
};


/**
 * What an object's material IS, independent of what windows or plays it
 * (proposal 37 D8b). Audio is sample data (and anything rendered from it);
 * Event is note/controller data. It decides which window type wraps a content
 * object (SClipWindow::wrapContent), and a take stack refuses to mix the two.
 *
 * Deliberately NOT a track kind: a track holds whatever clips it is given
 * (design D3), so nothing above the clip needs to branch on this.
 */
enum class SContentKind {
    Audio = 0,
    Event = 1
};


/**
 * This is QBX generic data container.
 * All data containers are children of the project object.
 * They linked together by SLink objects.
 *
 * All things marked as properties are user editable.
 */
class SObject
    : public QObject,
      public IRevalidatable   // engine-side revalidator contract (proposal 14)
{
    Q_OBJECT
    Q_PROPERTY( bool Solo READ isSolo WRITE setSolo )
    Q_PROPERTY( bool Muted READ isMuted WRITE setMuted )
    Q_PROPERTY( bool ArmedForRecording READ isArmedForRecording WRITE setArmedForRecording )
    Q_PROPERTY( double Volume READ getVolume WRITE setVolume )
    Q_PROPERTY( double Pan READ getPan WRITE setPan )
    Q_PROPERTY( double Delay READ getDelay WRITE setDelay )
    Q_PROPERTY( QString SName READ getSName WRITE setSName )
        
//Q_PROPERTY( type name READ getFunction [WRITE setFunction]
//            [STORED bool] [DESIGNABLE bool] [RESET resetFunction])


public:
    SObject( SProject* );
    virtual ~SObject();

    // WARNING: Returns reference. Will crash if parent is null.
    // CRITICAL: Destructors MUST use getProjectSafe() instead.
    // This method asserts if called during destruction (parent becoming invalid).
    SProject &getProject() const {
        Q_ASSERT_X(parent() != nullptr, "SObject::getProject",
                   "Parent project is null - destructor should use getProjectSafe()");
        return *(SProject *)parent();
    }

    // Safe accessor: returns pointer (may be null) for use during destruction.
    // Destructors MUST use this instead of getProject() to handle the case
    // where the SObject is being destroyed while its parent is invalid.
    SProject *getProjectSafe() const;

    virtual int serialize( QTextStream & );
    virtual int readPreChildrenAttributes( QDomElement &element );
    virtual int readPostChildrenAttributes( QDomElement &element );

    /**
     * Return the DSP root component from the SObject.
     * Its outputs may be connected in various ways, as the SObject
     * may be included at different parts of the entire arrangement.
     */
    virtual std::shared_ptr<twComponent> getRootComponent() = 0;

    /**
     * If this object is backed by random-access sample data, return that source
     * (proposal 07). Consumers use it to mint independent readers and to read
     * statelessly (e.g. preview rendering) without disturbing any play cursor.
     * The default returns NULL: most objects are not random-access sources.
     */
    virtual twRandomSource *getRandomSource() { return NULL; }

    /**
     * Return a widget suitable for full-screen editing this SObject.
     */
    virtual QWidget *getDetailEditWidget( QWidget *parent=NULL ) = 0;
    /**
     * Don't know what, but provided...
     */
    virtual QWidget *getInlineEditWidget( QWidget *parent=NULL ) = 0;
    
    virtual SObjectRenderer *getInlineRenderer() = 0;        

    /**
     * Seek the underlying twComponents to a given point, so that subsequent
     * playback will start there.
     * This method is provided inside the objects to facilate different underlying
     * interfaces.
     */
    virtual int seekTo( offset_t );

    /**
     * Map a clip-relative timeline position into this object's root component's
     * own position domain. Tracks compute clip-relative positions; a windowed
     * clip (SCut) plays a reader over the SOURCE material, so its slip offset
     * (grain-stretched as needed) must be folded in before the component is
     * seeked or page-frozen. Default is the identity: most objects' root
     * components are already clip-relative. Consumed via twView (see STrack).
     */
    virtual offset_t mapTimelineToComponentPos( offset_t off ) { return off; }

    /**
     * Proposal 19 Inv-1: resolve, from ONE structural snapshot, both the root
     * component to freeze/seek AND the timeline→component position mapping. For
     * a windowed clip (SCut/STakeStack) the component identity (built reader vs
     * shared content source) and the slip mapping both depend on the lazily
     * built reader; resolving them in separate getRootComponent() /
     * mapTimelineToComponentPos() calls can straddle a concurrent reader build
     * and disagree (the takes_group_broadcast race class). Overriders compute
     * both under one lock. The default is safe for objects whose mapping is the
     * identity and whose component is stable. Consumed via twView (see STrack).
     */
    virtual twResolvedClip resolveClip( offset_t off )
    { return twResolvedClip{ getRootComponent(), mapTimelineToComponentPos( off ) }; }

    /**
     * The EVENT twin of resolveClip (proposal 37 §4.2): the frame-domain event
     * sequence this clip contributes, plus the clip-relative position map the
     * track's twEventClipSet enumerates it through. The default returns an
     * empty record, which the clip set reads as "nothing to collect".
     *
     * It lives on SObject, not on a MIDI type, so `app/objects/track` can route
     * an event clip into its clip set without depending on `app/objects/midi`
     * (design §3.5: the track consults MIDI-ness only through contentKind()).
     * Resolved ONCE per collect, at the window start, exactly as twView::resolve
     * is for audio.
     */
    virtual twEventClipResolved resolveEventClip( offset_t clipPos )
    { (void) clipPos; return twEventClipResolved{}; }

    /**
     * The RESIDUAL event feed a clip contributes when PLACED (proposal 41
     * D4/M3), as distinct from resolveEventClip() above: a plain event clip
     * IS its own content, but a container that has no track identity (a lane
     * fragment) consumes nothing, so its whole event feed is residual and
     * must bubble into whatever track the placement sits on.
     *
     * The base default JOINS contentKind() and resolveEventClip() — for
     * ordinary Event-kind content this is already the right answer, so only a
     * content kind that is NOT itself Event but still carries events (a
     * fragment, via its SCut window) needs to override. It lives here, not on
     * a fragment-specific type, for the same reason contentKind() and
     * resolveEventClip() do: the track routes without knowing the subclass
     * (design 37 §3.5, extended by 41 D4).
     *
     * `clipPos` is unused by every override to date (SMidiCut's
     * resolveEventClip ignores it too) — the returned `map` carries the
     * slip/loop translation, not the argument.
     */
    virtual twEventClipResolved resolveEventFeed( offset_t clipPos )
    {
        return contentKind() == SContentKind::Event ? resolveEventClip( clipPos )
                                                      : twEventClipResolved{};
    }

    /**
     * True for a container whose own audio sum is EMPTY right now — a pure-
     * event lane fragment, or one with no children at all (proposal 41 D7/M4).
     * `SCut::buildCapture_` treats any no-random-source content as container-
     * backed and renders it into a snapshot on the UI thread; over a fragment
     * with no audio children that render is guaranteed silence, bought at
     * tens of milliseconds. Default false — everything before proposal 41
     * either has a random source or is a real audio-summing container.
     */
    virtual bool isPureEventContent() const { return false; }

    /**
     * One step of a WINDOW, for a position walk (proposal 09 §15).
     *
     * A window (a cut, a take stack) does not place its content by a start
     * time the way a container places a child link: it applies its own slip,
     * loop fold and stretch. So a walk that converts a position in THIS
     * object's timeline into a position in the material underneath it has to
     * ask the window, and this is where it asks.
     *
     * `clipRel` is a position in this object's own domain (already relative to
     * this object's start); on success `content` is the object the material
     * actually lives in and `pos` is the position IN THAT OBJECT. False means
     * "not a window" — the caller then walks childLinks() by start time, which
     * is what an ordinary container wants.
     *
     * It lives on SObject for the reason contentKind() and resolveEventClip()
     * do: the walk is in the MODEL and may not depend on `app/objects/cut`.
     * Read-only and non-blocking BY CONTRACT — a repaint calls it, so it must
     * never build a capture, acquire a reader or block on the object mutex
     * (main/timeline/CONTRACT.md inv. 1). SCut's override therefore takes the
     * try-lock snapshot, never getSnapshotBlocking().
     */
    struct SWindowStep {
        SObject *content = nullptr;
        offset_t pos     = 0;
    };
    virtual bool windowStep( offset_t clipRel, SWindowStep &out ) const
    { (void) clipRel; (void) out; return false; }

    /**
     * True for containers the index-path search may descend into (proposal
     * 41 D3: PATH DESCENT, not lane-ness). Path RESOLUTION follows explicit
     * indices and needs no flag; this only scopes the reverse search
     * (pathOf) exactly as the historical dynamic_cast<STrack*> did, and it
     * is also the general "may this be windowed as an asset / does this
     * childLink hold a nested container rather than a leaf clip" test used
     * throughout objects/track, objects/cut and objects/mixer.
     *
     * Until proposal 41 M1 every override of this was ALSO a lane (STrack,
     * SStdMixer), so callers that actually wanted "is this a lane" (solo,
     * mute, edit groups, arm, the active-lane map) read this flag too, and
     * the two meanings agreed by accident. M1 introduces the first
     * container that is NOT a lane (SLaneFragment: a path container with no
     * track identity — no fader, no inserts, no instrument, no solo, no
     * arm), so the accident stops holding. Lane STATE lives on isLane()
     * below; this flag stays purely about tree descent / "is this a
     * container, not a leaf". A lane answers both true; a fragment answers
     * this one true and isLane() false.
     */
    virtual bool isPathContainer() const { return false; }

    /**
     * True for objects that carry LANE state: solo, mute, edit-group
     * membership, arm-for-recording, and the active-lane / playhead-map
     * entries that key off a lane rather than off any path container
     * (proposal 41 D3). Every lane is a path container (it must be
     * descended into to resolve its own children's paths), but not every
     * path container is a lane — a fragment (proposal 41 M1) holds clip
     * links and answers isPathContainer() true, yet has no track identity
     * at all, so it must never be consulted for solo/mute/edit-group/arm:
     * doing so would let a fragment-internal flag darken lanes across the
     * whole project the fragment happens to be placed into.
     *
     * Defaults false. STrack and SStdMixer override it true, exactly as
     * they override isPathContainer() true — the two happened to answer
     * identically before proposal 41 and are now two separate questions
     * asked for two separate reasons.
     */
    virtual bool isLane() const { return false; }

    /**
     * True for a PACKED LANE FRAGMENT -- the container `pack-clips` mints and
     * `unpack-clips` takes apart (proposal 41 M1/M2).
     *
     * On the base class for exactly the reason contentKind(),
     * resolveEventClip() and isLiveRecording() are: the ARRANGER has to ask
     * the question (its context menu offers Unpack only over a fragment
     * placement) and `app/timeline` deliberately has no edge to
     * `app/objects/fragment` -- the tag chip already reads a fragment's name
     * through the type-agnostic SClipWindow interface for the same reason.
     *
     * NOT spelled `isPathContainer() && !isLane()`, which is true of a
     * fragment today and reads like a test for one. That conjunction is an
     * ACCIDENT of there being exactly one non-lane container so far, and
     * relying on an accidental agreement between two predicates is precisely
     * what proposal 41 M0 split them up to stop doing.
     */
    virtual bool isLaneFragment() const { return false; }

    /**
     * The kind of material this object carries (proposal 37 D8b). Audio by
     * default — every object that existed before event clips is audio, and a
     * container's kind is the kind of what it renders, which is audio too.
     * An event content object (SMidiSequence) and the window over it override.
     */
    virtual SContentKind contentKind() const { return SContentKind::Audio; }

    /**
     * True when this object is a PLACEHOLDER for an external file that could
     * not be loaded (see SPlainWave::setMissingWave). It has a duration, a
     * component and a path; what it does not have is audio.
     *
     * ON THE BASE CLASS for exactly the reason contentKind(),
     * resolveEventClip() and isLiveRecording() are: the CUT slice has to ask
     * this about its content without knowing which object slice that content
     * belongs to, and objects/cut has no edge to objects/wave.
     *
     * Load-bearing, not cosmetic. SCut treats "no random source" as
     * CONTAINER-BACKED and answers that by RENDERING the content into a
     * fixed-size capture — over a placeholder that is a full-length render of
     * silence, on the UI thread, once per clip. Measured on a project with
     * three unreachable samples: ~17 s of load, all of it producing zeros.
     */
    virtual bool isMissing() const { return false; }

    /**
     * Volume (dB) snapshot safe to take while audio runs / UI sliders move:
     * the read holds volumeMutex_ (the paint path races setVolume). Lets
     * renderers stay type-agnostic — the mutex was always SObject's, the
     * historical STrack cast in the waveform drawer was needless.
     */
    double volumeDbSnapshot() const;

    /**
     * The container's currently active/selected lane object (UI highlight),
     * or null. SStdMixer overrides with its selected track; generic so lane
     * renderers need no mixer type.
     */
    virtual SObject *activeLane() const { return nullptr; }

    /**
     * A column of ALTERNATIVE windows (a take stack) exposes its lanes here:
     * the window at `index`, or the ACTIVE one when index < 0. Null for
     * everything else, which is every object that is not a stack.
     *
     * It exists so a verb can address a take without naming STakeStack: a
     * slice at the rank of objects/cut (objects/midi) has no edge to it, and
     * the take rule is generic anyway - a stack is homogeneous by contentKind,
     * so whoever asks already knows what it will get back.
     */
    virtual SClipWindow *windowTakeAt( int index ) const
    { (void) index; return nullptr; }

    /**
     * The REST of that seam (proposal 21 L4), for the same reason and with the
     * same defaults: an object that is not a take column answers 0 / -1 /
     * null / nothing, and `add-midi-take` can build a column of EVENT takes
     * without `objects/midi` growing an edge to `objects/cut` (where
     * `STakeStack` lives). The stack is window-typed since proposal 37 D8b, so
     * everything below is already generic there - these are one-line
     * forwarders, not new behaviour.
     *
     * `insertWindowTake` returns the new take's link, or null when the column
     * REFUSED it (homogeneity: a stack of audio takes will not accept an event
     * window). Main thread only, like every other model mutation.
     */
    virtual int windowTakeCount() const { return 0; }
    virtual int activeWindowTakeIndex() const { return -1; }
    virtual SLink *insertWindowTake( SClipWindow &window, int atIndex )
    { (void) window; (void) atIndex; return nullptr; }
    virtual void removeWindowTake( int index ) { (void) index; }
    virtual void setActiveWindowTake( int index ) { (void) index; }

    // --- automation lanes (proposal 37 P5, design §3.3) --------------------
    //
    // OWNER-HELD, NEVER AN SLink CHILD. They live on SObject rather than on the
    // four owner types (STrack / SPluginSlot / SCut / SMidiCut) for the same
    // reason resolveEventClip() does: a verb, the serializer and the testkit
    // must reach a lane without knowing which slice its owner belongs to, and
    // `main/actions` may not depend on `objects/*` at all. WHICH targets are
    // legal on WHICH owner is validated by the verbs, not by the storage.
    //
    // The lanes vector is only ever touched on the MAIN thread (every verb, the
    // loader and the serializer run there); what crosses to a freeze thread is
    // the immutable twAutomationCurve SNAPSHOT, handed to the consuming
    // component under ITS mutex (THREADING rule 2).

    /// The lane for `target` (ParamRef spelling), or null.
    SAutomationLane *automationLane( const QString &target ) const;
    /// The lane for `target`, creating it if absent. Null only when the target
    /// does not parse.
    SAutomationLane *ensureAutomationLane( const QString &target );
    /// Drop the lane. Returns true when one was there.
    bool removeAutomationLane( const QString &target );
    /// Every lane, in insertion order.
    QList<SAutomationLane *> automationLanes() const;
    bool hasAutomationLanes() const { return !automationLanes_.empty(); }

    /// The snapshot a consumer should use for `target`: null when the lane is
    /// absent, empty or Off — and "null" is the SCALAR path, which is what keeps
    /// a project with no lanes byte-identical (P5 AC6).
    std::shared_ptr<const twAutomationCurve>
        automationCurve( const QString &target ) const;

    /// Called AFTER a lane mutation, with the affected range in THIS object's
    /// own time domain. Owners override to push the new snapshot into their
    /// engine components and to stale exactly that range
    /// (invalidateRenderPathRange). The default does the invalidation only —
    /// correct for an owner with nothing to push.
    virtual void onAutomationChanged( SAutomationLane &lane,
                                      offset_t start, offset_t end );

    /// Copy `src`'s lanes onto this object, replacing whatever is here. Used by
    /// the window CLONE path (duplicate-clip, add-take): a clip envelope lives
    /// on the window and therefore travels with every copy of it (design §3.3).
    void copyAutomationFrom( const SObject &src );

    /// Push every lane's current snapshot into the engine. Called after a load
    /// (the lanes are read before the components exist) and after any rebuild
    /// of the owner's chain. Default: nothing to push.
    virtual void applyAutomationToEngine() {}

    /**
     * Ordered view of this container's SLink children. Prefer this and the
     * childAt()/childCount() accessors over QObject::children() everywhere order
     * matters, so call sites stay decoupled from the storage.
     */
    SChildLinks childLinks() const { return SChildLinks( childOrder_ ); }

    /**
     * SLinks this object OWNS but which are NOT its ordered SLink children —
     * the references a container holds outside the document tree. Empty for
     * almost everything; STrack's reference to its SPluginChain is the one
     * case today, and it is deliberate (objects/track/CONTRACT.md 7: a chain
     * in childLinks() would be read as a clip).
     *
     * They are edges of the reference graph all the same, so anything that
     * reasons about WHO REFERENCES WHOM has to ask here as well as at
     * childLinks(). SProject::~SProject's survivor ordering is why this
     * exists: blind to this edge it deleted a referent before its referrer,
     * and the referrer's ~SLink then ran removeRef() on freed memory — the
     * "destroyed with N live reference(s)" teardown segfault.
     *
     * Main thread only, like every other lifetime operation (THREADING rule 1).
     */
    virtual QList<SLink *> ownedRefLinks() const { return QList<SLink *>(); }
    int childCount() const { return childOrder_.size(); }
    SLink *childAt( int index ) const;
    int indexOfChild( const SLink *child ) const;
    int indexOfChildObject( const SObject &child ) const;

    /**
     * Reorder this container's SLink children so the child currently at
     * fromIndex ends up at toIndex (the others shift to fill). No-op if either
     * index is out of range or they are equal. Order is just the explicit list,
     * so this is a plain list move — QObject parentage and refcounts are
     * untouched and no childObject signals fire.
     */
    void moveChildToIndex( int fromIndex, int toIndex );

    /**
     * Return the number of references to this object (from SLinks)
     */
    int getNReferences() const;
    
    /**
     * Returns true, if this object has no links to other objects.
     */
    virtual bool isEmpty() const;

    virtual bool hasDuration() const;
    virtual length_t getDuration() const;

    /**
     * Blocking variant of getDuration() for EDIT-path reads (proposal 19
     * Phase 2b, extended). SCut::getDuration() resolves through a try-lock
     * snapshot: when a background revalidation worker happens to hold the cut's
     * mutex, it returns the stale lastGoodSnapshot_ — on a freshly created cut
     * that is a DEFAULT snapshot with cutDuration 0. An edit-path consumer that
     * acts on that value bakes it in (e.g. STrack inserting a clip as
     * duration=0 = UNBOUNDED, which then bleeds source material past the clip
     * end — the takes_recording_placement doubling). Edit-path readers must use
     * this blocking read; only the RT audio path keeps the try-lock fallback.
     * Default: getDuration() (most objects read plain fields, no fallback).
     */
    virtual length_t getDurationBlocking() const { return getDuration(); }
    
    virtual bool hasPreview() const;
    virtual int getPreview( preview_t *dest,
                    offset_t start, length_t length,
                    offset_t nProbes );

    /**
     * Is this a LIVE RECORDING — a content (or a window over one) whose
     * material is still arriving from a capture device (proposal 21 L3b,
     * design D7)?
     *
     * On the base class for exactly the reason contentKind() and
     * resolveEventClip() are: the TRACK has to route by it (a growing clip has
     * no component and never enters the bus mixers) and `objects/track` may
     * not know the concrete type. `SCut` forwards its content's answer, so the
     * predicate holds for the recording CUT as well as for the content.
     */
    virtual bool isLiveRecording() const { return false; }

    /**
     * Phase 5e: Unified page cache API
     *
     * All SObjects use the same capture/revalidation system for preview,
     * playback, metadata, and export. This provides async, non-blocking
     * access to cached rendered output.
     *
     * Synchronization:
     * - currentPage_ pointer: atomic_load/store (lock-free reads)
     * - Page contents: protected by page->pageMutex
     * - Single mutex() per object: protects window parameters, not pages
     */

    // Invalidate specific capture aspects (Preview/Playback/Metadata/Export
    // bits from tw/schedule/capture_aspects.h). Base: no-op — objects without
    // a capture have nothing to invalidate. SCut overrides to drop stale
    // pages and schedule async revalidation. Virtual so dependency
    // notification needs no knowledge of concrete types (proposal 14, Phase 5).
    virtual void invalidateAspects( uint32_t aspects ) { (void) aspects; }

    // Non-blocking access to capture page (may be stale or invalid).
    // Returns immediately with current page if valid, or nullptr if not ready.
    // Never blocks on revalidation; schedules it if needed.
    // Returns stale data if available (acceptable for UI preview).
    std::shared_ptr<CapturePageData> getCapture(uint32_t aspectsMask);

    // Get current page without locking. Safe because shared_ptr copy is atomic
    // (uses std::atomic_load internally).
    std::shared_ptr<CapturePageData> currentPage() const {
        return std::atomic_load(&currentPage_);
    }

    // Check if revalidation is needed for specific aspects.
    // _nolock: caller must hold mutex() before calling.
    // Internal version used by revalidator when processing jobs.
    bool needsRevalidation_nolock(uint32_t aspectsMask) const;

    /**
     * Abstract interface for revalidation (Phase 5e).
     *
     * Each SObject type implements these to compute capture data.
     * Called by CaptureRevalidator worker threads with page lock held.
     *
     * Implementation notes:
     * - Called outside audio thread; can block
     * - Revalidator guarantees page is exclusive (not visible to readers)
     * - Aspects indicate which fields caller wants computed
     * - Set page.validAspects |= computedAspects before returning
     *
     * Note: recomputePreview() removed (Phase 5 — now uses freezePreviewPage)
     */
    virtual void recomputeMetadata(CapturePageData& page) {}
    virtual void recomputeExport(CapturePageData& page) {}

    // IRevalidatable (the engine-side revalidator contract): thin delegations
    // to the members above, preserving the historical dispatch exactly — the
    // *_nolock page methods bind statically to SObject's own implementations
    // (as the revalidator's SObject* calls always did), while the recompute
    // hooks and getRootComponent() stay virtual per object type.
    std::mutex &revalMutex() const override { return mutex(); }
    // The revalidator pin is NOT the Qt refcount: pins are taken/released on
    // WORKER threads (scheduleRevalidation re-queues and every job-exit path
    // unpins on the worker), while addRef()/removeRef() are main-thread-only
    // (non-atomic count, Qt signals, deleteLater). Routing pins through the
    // refcount raced the ++/-- and could corrupt nRefs_ — a premature
    // deleteLater() then destroyed an object that live SLinks still pointed
    // at (the vtable-garbage paint crash). Pins are a separate atomic; while
    // any pin is held a refcount-driven deletion is deferred (see event()),
    // and the last unpin re-arms it.
    void revalAddRef() override;
    void revalRemoveRef() override;
    bool revalNeeded_nolock(uint32_t aspects) const override
        { return needsRevalidation_nolock(aspects); }
    std::shared_ptr<CapturePageData> revalGetNextPage_nolock() const override
        { return getNextPage_nolock(); }
    void revalSetNextPage_nolock(std::shared_ptr<CapturePageData> page) override
        { setNextPage_nolock(page); }
    void revalSwapPages_nolock() override { swapPages_nolock(); }
    std::shared_ptr<twComponent> revalRootComponent() override { return getRootComponent(); }
    void revalRecomputeMetadata(CapturePageData &page) override
        { recomputeMetadata(page); }
    void revalRecomputeExport(CapturePageData &page) override
        { recomputeExport(page); }
    // Worker thread → queued UI repaint when a revalidation lands (Phase 5e.6).
    void revalCompleted(uint32_t aspects) override;

    // Edit group (proposal 17 phase 4): tracks sharing a nonzero id form one
    // ARBITRARY group set (not tied to the hierarchy); clip edits on one
    // member broadcast to the corresponding clips of the others (see
    // seditgroups.h and the `broadcast` attribute on the clip verbs).
    int getEditGroup() const
        { return editGroup_; }

    // User properties.
    bool isSolo() const
        { return solo_; }
    bool isMuted() const
        { return muted_; }
    bool isArmedForRecording() const
        { return armed_; }
    // Recording channel selection (bitmask: 0 = all channels, 1<<n = channel n).
    // The DEFAULT is bit 0 — the interface's first input only. It used to be 0
    // ("all"), which on a 16-input interface wrote a 16-channel file with the
    // signal in channel 0 and a dither floor in 1..15; since proposal 36 B3 a
    // reader keeps its file's width, so a 2-channel track then took channel 1
    // of that file and played a dead right speaker (see
    // plan/todo/RECORDING_CHANNEL_COUNT.md). "All Channels" stays reachable
    // from the ARM button's right-click menu for a live multitrack capture.
    uint32_t getRecordingChannels() const
        { return recordingChannels_; }
    // Written to the project file only when it differs from this, so projects
    // saved before the selection was serialized stay byte-unchanged.
    static constexpr uint32_t DEFAULT_RECORDING_CHANNELS = 1u;
    double getVolume() const
        { return volume_; }
    double getPan() const
        { return pan_; }
    double getDelay() const
        { return delay_; }
    QString getSName() const
        { return sName_; }
    // Every SObject is born with this name (see the ctor), so it is a display
    // PLACEHOLDER that happens to live in storage — not something the user
    // chose. serializeSelfAttributes() therefore skips it, or every object in
    // every project file would carry a meaningless sName attribute.
    static constexpr const char *DEFAULT_SNAME = "(untitled)";

public slots:
    void setSolo( bool );
    void setMuted( bool );
    void setArmedForRecording( bool );
    void setEditGroup( int );
    void setRecordingChannels( uint32_t channels );
    void setVolume( double );
    void setPan( double );
    void setDelay( double );
    void setSName( const QString & );    

signals:
    // For the properties
    void soloChanged( bool );
    void mutedChanged( bool );
    void armedForRecordingChanged( bool );
    void editGroupChanged( int );
    void recordingChannelsChanged( uint32_t );
    void volumeChanged( double );
    void panChanged( double );
    void delayChanged( double );
    void sNameChanged( const QString & );


    /**
     * Ths link's duration changed.
     */
    void durationChanged( length_t newDuration );

    /**
     * This object's EVENT content changed from `fromClipPos` (clip-relative
     * frames) onward — a note edit, a window edit, a tempo re-map. The owning
     * container touches its event clip set and invalidates [from, INF) (design
     * §3.2: the consumer is class-1, so a change is never bounded on the right).
     *
     * Declared here rather than on SMidiCut so `app/objects/track` can connect
     * to it without an edge to `app/objects/midi`.
     */
    void eventsChanged( offset_t fromClipPos );

    /**
     * Child object was added.
     */
    void childObjectAdded( SLink &child );
    
    /**
     * Child object was removed.
     * At calling time, the link is not yet deleted.
     */ 
    void childObjectRemoved( SLink &child );

    /**
     * This signal is emitted, if the object becoms unreferenced.
     */
    void gotUnreferenced();

    /**
     * This signal is emitted, if the object receives its first reference.
     */
    void gotReferenced();

    void nRefsChanged();

public slots:
    /**
     * Set a different duration.
     */
    virtual void setDuration( length_t );

    /**
     * Add a reference.
     */
    void addRef();

    /**
     * Remove a reference.
     */
    void removeRef();

    /**
     * Forget the current preview.
     */
    virtual void invalidatePreview();

    /**
     * Notify dependents (objects that reference this one) that specific aspects
     * have changed. Only affected dependents are invalidated (lazy invalidation).
     * Called by audio state changes (mute, solo, volume) that don't affect arrangement.
     * Example: setMuted() → notifyDependentsChanged(Playback | Metadata)
     */
    void notifyDependentsChanged(uint32_t affectedAspects);

    /**
     * Register a dependent link (object that references this one via SLink).
     * Called when an asset is placed or a cut references this object.
     * Uses SLink (the native reference primitive) to track who depends on this object.
     */
    void addDependentLink(SLink *dependentLink);

    /**
     * Unregister a dependent link. Called when a placement or cut is removed.
     */
    void removeDependentLink(SLink *dependentLink);

    // --- Scoped render-cache invalidation (proposal 15) -------------------
    // An edit must stale the frozen pages of the edited object's engine chain
    // and every container on the path(s) to the root — and nothing else, so
    // sibling tracks' caches (and reused material behind other paths) survive.

    /**
     * Bump the content epoch of THIS object's engine components (track mixers,
     * plugin chains, rewire — whatever this object owns). Default: no-op;
     * overridden by lane containers (STrack, SStdMixer).
     */
    virtual void bumpRenderChainEpoch() {}

    /**
     * Walk this subtree; if it contains `target` (or IS the target), bump this
     * object's render chain. Visits every path, so material linked under
     * multiple parents invalidates all of its containers. Returns whether the
     * subtree contains the target.
     */
    bool invalidateRenderChainsContaining(SObject *target);

    /**
     * Entry point after an edit on this object: bump the render chains on
     * every path from the project root down to this object. Call AFTER the
     * engine-side mutation (clip list, wiring) so a freeze racing the edit is
     * re-rendered rather than a pre-edit freeze being stamped current.
     */
    void invalidateRenderPath();

    /**
     * How many SLinks (plus any explicit pin, e.g. SProject::registerAsset's
     * addRef()) currently reference this object — main-thread read of the
     * same counter addRef()/removeRef() maintain (proposal 41 M2). A
     * registered asset's body sits at 1 with no placement; a lane-fragment
     * unpack refuses unless it reads exactly 2 (the registry pin plus the
     * ONE placement being unpacked), so unpacking can never empty a fragment
     * out from under a SECOND placement still sharing it (D2's invariant).
     */
    int refCount() const { return nRefs_; }

    // --- Range-scoped variant (proposal 18 Phase 5) -------------------------
    // Plain methods, not slots (moc must not see the struct declaration).
public:
    // A dirty interval [start, end) in SOME object's own timeline domain.
    struct SDirtyRange {
        offset_t start;
        offset_t end;
    };

    /**
     * Range-scoped bump of THIS object's engine components: only pages
     * intersecting [start, end) (this object's timeline domain) go stale.
     * Default falls back to the whole-chain bump; lane containers override
     * with twComponent::invalidatePagesInRange.
     */
    virtual void bumpRenderChainEpochRange( offset_t start, offset_t end ) {
        (void) start; (void) end;
        bumpRenderChainEpoch();
    }

    /**
     * Translate dirty ranges from a child's timeline domain into THIS
     * object's domain. Default: the containment ShiftMap (+ the link's
     * startTime, saturating). SCut overrides with the full window mapping
     * (stretch, slip, loop tiling preimages); STakeStack drops ranges from
     * inactive takes. May return FEWER ranges than given (an edit outside
     * the audible window dirties nothing here or above).
     */
    virtual QList<SDirtyRange> mapChildRangesToSelf(
        SLink *childLink, const QList<SDirtyRange> &childRanges );

    /**
     * Range-carrying version of invalidateRenderChainsContaining: the walk
     * finds `target`, then maps [targetStart, targetEnd) upward through
     * each containment hop, bumping every container ONLY over the mapped
     * ranges. `rangesInSelf` returns the dirty ranges in THIS object's
     * domain (may be empty even when the subtree contains the target — the
     * edit can be windowed away).
     */
    bool invalidateRenderChainsContainingRange(
        SObject *target, offset_t targetStart, offset_t targetEnd,
        QList<SDirtyRange> &rangesInSelf );

    /**
     * Entry point after an edit affecting [start, end) of THIS object's
     * timeline: stale exactly the affected page ranges on every path from
     * the project root down to this object. Same ordering contract as
     * invalidateRenderPath (call AFTER the engine mutation).
     */
    void invalidateRenderPathRange( offset_t start, offset_t end );

    // Helper methods for revalidator integration (Phase 5e).
    // _nolock suffix indicates caller MUST hold mutex() before calling.
    // These are friends-only methods, non-locking to avoid recursive lock deadlock.
    friend class CaptureRevalidator;

    // Atomic swap pages. _nolock: caller must hold mutex()
    // Uses std::atomic_store for thread-safe write (pairs with atomic_load in currentPage()).
    void swapPages_nolock() {
        std::atomic_store(&currentPage_, nextPage_);
        nextPage_ = nullptr;
    }

    // Get next capture page. _nolock: caller must hold mutex()
    std::shared_ptr<CapturePageData> getNextPage_nolock() const {
        return nextPage_;
    }

    // Set next capture page. _nolock: caller must hold mutex()
    void setNextPage_nolock(std::shared_ptr<CapturePageData> page) {
        nextPage_ = page;
    }

protected:
    /**
     * Thread safety: all derived classes use this mutex to protect their state.
     * Single mutex per object; see async_revalidation_phase4.md for rationale.
     * Usage: std::lock_guard<std::mutex> lock(mutex());
     */
    std::mutex& mutex() const {
        return stateMutex_;
    }

    offset_t getChildrenExtent( offset_t &firstStart, offset_t &lastEnd,
                                int &nUndefStart, int &nUndefDuration ) const;
    offset_t getFirstChildStartTime() const;
    length_t getAllChildsDuration() const;

    int getStraightPreview( preview_t *, offset_t, length_t, offset_t );
    virtual void childEvent( QChildEvent * ) override;
    // Swallows a pending DeferredDelete when the object was re-referenced
    // after removeRef() hit zero (deleteLater() cannot be rescinded; without
    // this, any 1->0->1 refcount transition destroys an object that live
    // SLinks still point at — vtable-garbage crash at the next paint).
    virtual bool event( QEvent * ) override;

    virtual int serializeSelfAttributes( QTextStream &o );

    // Emit `<automation>…</automation>` (nothing at all when there are no
    // lanes, so every project written before P5 is byte-unchanged). Called
    // from SObject::serialize() and from every serialize() override that
    // writes its own children (SPluginSlot's `<state>`).
    int serializeAutomation( QTextStream &o );

    /**
     * Inline non-`SLink` children beyond `<automation>`, written in
     * `serialize()` right after it and read back by the owner's own
     * `readPostChildrenAttributes`.
     *
     * It exists for the same reason `<automation>` is inline rather than an
     * `SLink` child (proposal 37 P5): the loader orders and resolves on
     * `<SLink>` children only, so an inline child of a known element is
     * invisible to it and an OLDER build ignores it. An owner that has none
     * writes nothing, which is what keeps every existing file and golden
     * byte-unchanged.
     */
    virtual int serializeInlineChildren( QTextStream &o ) { (void) o; return 0; }
    // Read the inline `<automation>` child. Tolerant: an unparsable target is
    // skipped with a warning, never a load failure.
    int readAutomation( const QDomElement &element );

    int getChildIndex( SObject & ) const;

protected:
    // Thread safety: mutex for all derived class state (single mutex per object).
    // Mutable so const methods can lock. Protected by mutex() accessor.
    // All derived classes should protect their state with this mutex.
    mutable std::mutex stateMutex_;

    // Main-thread only (see the automation block above).
    std::vector<std::unique_ptr<SAutomationLane> > automationLanes_;

    // Phase 5e: Page cache infrastructure (unified across all SObjects).
    // Two-page buffer model (Unix page cache pattern):
    // - currentPage_: visible to readers (via atomic_load)
    // - nextPage_: being built by revalidator (exclusive, not yet visible)
    // When revalidator finishes, it atomic_swaps them.
    //
    // Access synchronization:
    // - Pointer itself: atomic_load/store (no mutex needed)
    // - Page contents: protected by page->pageMutex
    // - Window parameters (startOffset, duration, etc.): protected by stateMutex_
    std::shared_ptr<CapturePageData> currentPage_;
    std::shared_ptr<CapturePageData> nextPage_;

    // Revalidator: borrowed from SProject (not owned).
    // Spawned background threads that build pages asynchronously.
    CaptureRevalidator* revalidator_ = nullptr;

    // Bitmask tracking which aspects are valid in currentPage_.
    // Updated by revalidator when pages are swapped and marked complete.
    uint32_t validAspects_ = 0;

    // Proposal 27 (M0): straight-preview sidecar hooks. An object backed by
    // content-hashable material (SPlainWave) overrides these to persist and
    // restore the straight preview across sessions. The geometry is passed
    // explicitly so this base class stays free of any sidecar dependency.
    // Both run on the UI thread, inside straightCalcPreviewData(), i.e. with
    // exactly the thread affinity previewData_ already has. Defaults: no
    // sidecar (fetch misses, store no-ops) — behavior identical to before.
    virtual bool fetchPreviewSidecar( preview_t *dest, offset_t nProbes,
                                      offset_t skip, offset_t forLength );
    virtual void storePreviewSidecar( const preview_t *data, offset_t nProbes,
                                      offset_t skip, offset_t forLength );

private:

    void gotChild( SLink & );
    void lostChild( SLink & );
    int straightCalcPreviewData();
    // Source of truth for child order (membership mirrors QObject::children(),
    // maintained in childEvent(); order is independent and set by
    // moveChildToIndex()).
    QList<SLink*> childOrder_;

    // Lazy invalidation + dependency tracking (proposal 06).
    // Set of SLink objects that reference this object (the native way to track references).
    // When this object's state changes, dependent links are notified.
    // Example: track output is captured by a cut link; edit track mute → cut's
    // Playback aspect invalidated, not entire scene.
    mutable std::mutex dependentsMutex_;
    QSet<SLink*> dependentLinks_;
    int nRefs_;
    // Revalidator keep-alive pins (worker-thread safe — see revalAddRef()).
    // deletePending_ records a refcount-driven deleteLater() that arrived
    // while pinned, so the last unpin can re-arm it.
    std::atomic<int> revalPins_{0};
    std::atomic<bool> deletePending_{false};
    offset_t previewForLength_;
    offset_t nPreviewProbes_;
    preview_t *previewData_;
    offset_t previewSkip_;

    bool solo_;
    bool muted_;
    bool armed_;
    int editGroup_ = 0;   // 0 = ungrouped (proposal 17 phase 4)
    double volume_;
    // Recording channel selection: bitmask of channels (bit 0 = ch 0, etc).
    // 0 means "all channels"; the default is the first input alone — see
    // getRecordingChannels(). Set via setRecordingChannels().
    uint32_t recordingChannels_ = DEFAULT_RECORDING_CHANNELS;

    // Thread-safe state: audio thread may read volume while UI thread modifies it.
    // Made public so preview rendering can snapshot the volume safely.
public:
    mutable std::mutex volumeMutex_;
private:
    double pan_;
    double delay_;
    QString sName_;
};

#endif
