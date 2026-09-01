
#ifndef _STRACK_H_
#define _STRACK_H_

#include <qobject.h>
#include <qlist.h>
#include <memory>
#include <string>
#include "app/model/sobject.h"
#include "app/model/sobjectrenderer.h"   // SEnvelopeWindow, preview_t
#include "tw/events/tweventclipset.h"
#include "tw/events/tweventmerge.h"

class twComponent;
class STrack;
class SObjectRenderer;
class STrackRendererInline;
class SPluginChain;
class SPluginSlot;
class SFeelFlowTrackBounce;
struct SFeelFlowUiData;
struct twGrooveTrainedStructure;
class twTrackMix;
class twRewire;
class twGainStage;
class twPluginChain;
class SLink;
class SProjectLoader;

class SStartTimeList
    : public QList<SLink*>
{
public:
    SStartTimeList();
    virtual ~SStartTimeList();    
protected:
    //virtual int compareItems( QCollection::Item , QCollection::Item );

private:
};

class SEndTimeList
    : public QList<SLink*>
{
public:
    SEndTimeList();
    virtual ~SEndTimeList();
protected:
    //virtual int compareItems( QCollection::Item , QCollection::Item );

private:
};

/**
 * A track is a helper class for SStdMixer.
 * To gain the effect of inserting a track twice, place
 * Them in two SStdMixers.
 */
class STrack
    : public SObject 
{
    Q_OBJECT
public:
    STrack( SProject *project );
    virtual ~STrack();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader, 
					     QDomElement &element, 
					     SObject *parent );

    virtual std::shared_ptr<twComponent> getRootComponent() override;

    /**
     * Our reference to the plugin chain (cpPluginChainRef_) is an owned SLink
     * that is deliberately NOT a child link, so childLinks() cannot see the
     * track -> chain edge. Publish it here or the reference graph is wrong for
     * everyone who walks it — ~SProject's survivor ordering deleted the chain
     * first and ~STrack's `delete cpPluginChainRef_` then removeRef()'d freed
     * memory.
     */
    QList<SLink *> ownedRefLinks() const override;

    virtual int readPreChildrenAttributes( QDomElement &element ) override;
    virtual int readPostChildrenAttributes( QDomElement &element ) override;

    // Proposal 40 M3: a full override (not just serializeSelfAttributes) --
    // trained-mode state is an INLINE element (the automation-lane
    // discipline: a non-<SLink> child the loader's ordering never sees), and
    // that has to land between the ">" and the child links, exactly where
    // SObject::serialize() already puts serializeAutomation(). See
    // strack.cpp for why this mirrors, rather than calls into a shared hook.
    virtual int serialize( QTextStream &o ) override;

    virtual QWidget *getDetailEditWidget( QWidget *parent ) override;
    virtual QWidget *getInlineEditWidget( QWidget *parent ) override;
    virtual SObjectRenderer *getInlineRenderer() override;
    
    virtual SLink *getTopMostSLinkAt( offset_t ) const;

    // How many CHANNELS this track's components are wide (proposal 36 B4).
    // There is no bus count any more: a track is ONE twTrackMix + ONE
    // twPluginChain + ONE twRewire, and this is the width of the pages they
    // freeze. It follows the PROJECT's channels= and nothing else.
    //
    // (M1's setNBussesCallCount() test hook lived here, and its own comment
    // said "retire it when B4 makes bus width a real consequence of project
    // width". This is that milestone: the claim it guarded — that the project's
    // channel count reaches no bus count — is now false on purpose, so a
    // counter that could only ever read zero would be worse than nothing.)
    int getChannels() const { return channels_; }

    SPluginChain *getPluginChain() const { return cpPluginChain_; }

    // --- system role (proposal 45 D1) -------------------------------------
    //
    // A SYSTEM LANE is an ordinary track the PROJECT owns rather than the
    // user: the post-sum master, a send destination, or a conductor lane
    // (tempo, time signature, markers) hanging off the master. It is not a
    // new type -- `dynamic_cast<STrack *>` appears 98 times across 40 files
    // in main/, and every one of them is a site a second lane type would
    // have to be audited at. Being an STrack is what makes the plugin chain,
    // the gain stage, metering, automation, the row and the head work with
    // no change of their own.
    //
    // IMMUTABLE: set once, at construction, by whoever mints the lane. A
    // lane does not become the master. There is deliberately no verb and no
    // undo entry for it.
    void setSystemRole( SSystemRole r ) { systemRole_ = r; }
    SSystemRole systemRole() const override { return systemRole_; }

    // A system lane carries no clips of its own (proposal 45 D6): a master
    // track must not directly hold sample or event material, though it may
    // hold child lanes. Enforced at ONE narrow seam,
    // splacements::placementLaneAt().
    bool acceptsClips() const override { return systemRole_ == SSystemRole::None; }

    // --- events (proposal 37 3.2 / 3.2.1) ---------------------------------
    //
    // A track holds no KIND (D3): it takes whatever clips it is given, and it
    // routes them by their material. An `SObject::contentKind() == Event`
    // child goes into THIS set — same slots, same `SLink*` key rule as the bus
    // mixers (CLIP_MODEL "identity is the SLink pointer") — and NOT into the
    // mixers, so a MIDI clip costs no dummy freeze per page. Nothing here
    // names a MIDI type: the content answers `contentKind()` and
    // `resolveEventClip()`, which is why `objects/track` has no edge to
    // `objects/midi`.

    /** This track's own event clips. Never null. */
    const std::shared_ptr<twEventClipSet> &eventClips() const
        { return eventClips_; }

    /**
     * THE FEED (3.2.1): this track's own set merged with the feeds of every
     * child track that passes events up. Events bubble like audio sums, under
     * one rule — a child passes up iff it neither consumes them locally (an
     * instrument slot, a MIDI-out port) nor is told otherwise by its
     * serialized `midiRouting`:
     *
     *   auto   — the rule above (the default; a folder with a drum machine on
     *            it plays its children's patterns, REAPER's behaviour);
     *   parent — force bubbling even past a local consumer;
     *   none   — keep them local.
     *
     * A child that is muted or solo-excluded contributes NOTHING (the same
     * ssolo resolution the summing container uses for audio). Clip-level mute
     * is already inside the clip set.
     *
     * The merge OBJECT is stable — a consumer may hold it — while its source
     * list is rebuilt on every call. Rebuilding is O(children) and always
     * correct; a dirty flag would have to be poked from a solo change anywhere
     * in the project, which is precisely the coupling ssolorules.h exists to
     * avoid.
     */
    std::shared_ptr<twEventMerge> eventFeed();

    /** True while any Event child is placed here. Derived, never stored. */
    bool hasEventClips() const;
    /** Slot 0 of the plugin chain when it carries an instrument, else null. */
    SPluginSlot *instrumentSlot() const;
    /**
     * The end of the last EVENT material this track's feed carries, in timeline
     * frames: our own Event children, plus the same question asked of every
     * child track that bubbles events up. 0 when there is none.
     *
     * Separate from getDuration()'s audio extent because the project end of an
     * instrument track is "last event clip end + tailFrames()" (design 3.1) —
     * an audio clip must not gain a synth's release, and a MIDI clip must not
     * lose it.
     */
    offset_t eventEndTime() const;
    /** Whether this track sends its feed to a MIDI port (proposal 37 P7b). */
    bool hasMidiOut() const { return !midiOutPort_.isEmpty(); }

    // --- MIDI output (D6 / P7b) -------------------------------------------
    //
    // A PORTABLE port NAME, never a machine-local device id: the id a WinMM
    // index or a CoreMIDI uniqueID gives is meaningless on the next machine,
    // so the project stores the name and `SSettings` resolves it per machine
    // (the same split the audio device selection uses). Empty = no MIDI out.
    //
    // `midiOutChannel` is 0-BASED (0..15), matching `twEvent::channel` and the
    // `add-note channel=` attribute, so the whole scripting API speaks one
    // convention; -1 means "as authored" (each event keeps its own channel).
    //
    // `midiOutOffsetMs` is a signed per-track send offset, +-500 ms, POSITIVE =
    // send EARLIER (the requester's outboard-gear case: gear whose audio return
    // arrives late is compensated so its audio lands on the grid).
    const QString &getMidiOutPort() const { return midiOutPort_; }
    int getMidiOutChannel() const { return midiOutChannel_; }
    int getMidiOutOffsetMs() const { return midiOutOffsetMs_; }
    // ABSOLUTE, like every other track flag: all three at once, clamped.
    void setMidiOutput( const QString &port, int channel, int offsetMs );
    static constexpr int MIDI_OUT_MAX_OFFSET_MS = 500;

    /** How this track's events reach its parent (3.2.1). Serialized. */
    enum class MidiRouting { Auto = 0, Parent = 1, None = 2 };
    MidiRouting getMidiRouting() const { return midiRouting_; }
    void setMidiRouting( MidiRouting );
    static MidiRouting midiRoutingFromString( const QString &, bool *ok = nullptr );
    static QString midiRoutingToString( MidiRouting );
    /** Does this track hand its events to its parent right now? */
    bool bubblesEventsUp() const;

    // --- live input / monitoring (proposal 21 L1b, design D9) --------------
    //
    // ONE per-track input selector, spelled as a PORTABLE string exactly the
    // way `midiOutPort` is a portable name: the machine-local half (a WASAPI
    // endpoint id, a WinMM index) never enters the project file.
    //
    //     none                     the default; the track has no live input
    //     audio:<device>:<mask>    an audio device, <mask> a hex channel mask
    //                              ("3" = the first two channels); <device>
    //                              empty = the app's selected input device
    //     midi:<port>:<ch|any>     a MIDI port and channel (consumed in L2)
    //     keyboard                 the computer keyboard port (L2)
    //
    // Kept as the raw string rather than a parsed struct because the model's
    // job is to round-trip it: the live plan builder parses it once, on the
    // main thread, and an unknown spelling must survive a save/load rather
    // than be silently normalised away.
    const QString &getTrackInput() const { return trackInput_; }
    void setTrackInput( const QString &spec );
    bool hasTrackInput() const
    { return !trackInput_.isEmpty() && trackInput_ != QStringLiteral( "none" ); }
    /** The `audio:` device id of trackInput_, or a null string. */
    QString trackInputAudioDevice() const;
    /** The `audio:` channel mask of trackInput_; 0 when there is none. */
    unsigned trackInputChannelMask() const;
    /**
     * The portable MIDI PORT NAME of trackInput_ (proposal 21 L2), or a null
     * string when the input is not a MIDI one.
     *
     * `keyboard` is spelled without a scheme and resolves to the computer
     * keyboard's own port id, so the two MIDI spellings differ only in how the
     * port is named - which is what makes the whole live-instrument path treat
     * the keyboard exactly like hardware (design D9).
     */
    QString trackInputMidiPort() const;
    /** The `midi:` channel, 0-BASED, or -1 for `any` / not a MIDI input. */
    int trackInputMidiChannel() const;
    /** Is this a `midi:` or `keyboard` input (the L2 half)? */
    bool hasMidiTrackInput() const { return !trackInputMidiPort().isNull(); }

    // Off / On / Auto, design D9. AUTO is TAPE-MACHINE style (Cubase
    // "Tapemachine", REAPER "auto"): the input sounds while the transport is
    // STOPPED or RECORDING and gives way to the track's own material on plain
    // Play. ON always monitors; OFF never does. Serialized only when it is not
    // the default.
    enum class MonitorMode { Auto = 0, On = 1, Off = 2 };
    MonitorMode getMonitorMode() const { return monitorMode_; }
    void setMonitorMode( MonitorMode );
    static MonitorMode monitorModeFromString( const QString &, bool *ok = nullptr );
    static QString monitorModeToString( MonitorMode );
    /**
     * Should this track monitor its input right now?
     *
     * `playing` is the transport state and `recording` says a record pass is
     * running; the two are separate because Auto follows the tape machine:
     * input while STOPPED or RECORDING, playback on plain Play.
     */
    bool monitorEffective( bool playing, bool recording ) const;

    // THE LIVE-OWNED PREDICATE (design D3). It is a WIRING predicate and
    // NOTHING else: the mixer nulls the plug of a live-owned top-level track
    // and a folder `setClipMuted`s a live-owned child, exactly the way solo
    // does it — and it is deliberately kept OUT of `ssolo::isLaneAudible`,
    // because folding it in there would drop a live child's EVENTS from a
    // folder instrument's feed and darken its meters.
    //
    // Set by SLivePlanBuilder on every member of the live closure BEFORE the
    // plan is published, cleared after the plan that drops it retires. Never
    // serialized: it describes this session's monitoring, not the project.
    bool isLiveOwnedLane() const { return liveOwnedLane_; }
    /// Clearing it FLUSHES the deferred invalidation (see
    /// invalidateRootWalkOrDefer): design D7's "the track's clip-sync
    /// suppresses the walk for a live-owned track and issues ONE at disarm".
    void setLiveOwnedLane( bool owned );

    /**
     * Stale this chain and every container up to the root over [start, end) —
     * UNLESS this lane is live-owned, in which case the range is accumulated
     * and ONE walk is issued when it is handed back (proposal 21 L3b, design
     * D7).
     *
     * The walk is the expensive half of a clip edit: it runs from the project
     * root down, per hop, mapping domains. While a lane is live-owned its
     * frozen output is not being summed at all — the pump renders it — so a
     * walk per edit buys nothing and a clip whose length moves ten times a
     * second would re-stale the root ten times a second.
     */
    void invalidateRootWalkOrDefer( offset_t start, offset_t end );

    // The engine pieces the live plan needs, in render order. They are handed
    // out as shared_ptr because the plan outlives any single call and the pump
    // reads them from another thread; the plan holds no raw component pointer
    // except the frozen-input roots it reads pages off.
    const std::shared_ptr<twTrackMix>  &trackMixComponent() const { return cpTrackMix_; }
    const std::shared_ptr<twGainStage> &gainStageComponent() const { return cpGainStage_; }
    const std::shared_ptr<twPluginChain> &pluginChainComponent() const { return cpDspChain_; }

    // Adopt a plugin chain loaded from a project file (proposal 08 M4): drop the
    // empty one the constructor made, take over the loaded one, reconnect its
    // signals and rebuild the track's DSP chain from its slots. (Said "the
    // per-bus DSP chains"; a track has had ONE twPluginChain of the project's
    // width since proposal 36 B4.) Called from the loader's deferred-resolve
    // pass, never during normal editing.
    void adoptPluginChain( SPluginChain *chain );
    virtual int seekTo( offset_t ofs ) override;

    // --- Feel Flow track bounce (proposal 40 M1b, section 4.3) ------------

    /**
     * Starts (or restarts) a background bounce+groove-analysis pass over
     * this track's own root component (post-FX, post-gain, pre-summing —
     * getRootComponent()), covering the whole project duration. Lazily
     * creates the holder (sfeelflowbounce.h). No-op, logged, when the
     * sidecar store is disabled or the project has no revalidator
     * (SMARAGD_REVAL_WORKERS=0); a no-op when a bounce for this track is
     * already running. Never blocks — the render and the analysis job that
     * follows it both run off the calling thread.
     */
    void startFeelFlowBounce();

    // True while a bounce (or the analysis job that follows it) for this
    // track is in flight. Lock-free badge read, mirroring
    // SPlainWave::isAnalyzingGroove().
    bool isFeelFlowBouncing() const;

    // True once a bounce has completed at least once for this track.
    bool feelFlowHasResult() const;

    // True when this track's chain has changed (its root component's
    // content epoch has moved) since the START of the last successful
    // bounce, or no bounce has ever completed — never silently reports a
    // result that no longer matches the chain (proposal 40 section 4.3).
    bool feelFlowStale() const;

    // Proposal 40 M2: the compliance heatmap's UI-cache read, forwarded to
    // the holder (SFeelFlowTrackBounce::feelFlowForUi() -- see its doc for
    // the onsetsForUi() discipline). Returns nullptr ONLY when no bounce has
    // EVER been started for this track (the holder itself does not exist
    // yet); once a holder exists this always returns a non-null result,
    // possibly empty (hopFrames == 0). Freshness is a SEPARATE question,
    // deliberately not folded in here -- a painter checks feelFlowStale()
    // itself, exactly as it already does for the badge.
    std::shared_ptr<const SFeelFlowUiData> feelFlowForUi() const;

    // --- Feel Flow tuning panel + trained mode (proposal 40 M3) -----------

    // Two modes, one switch (design section 3.2): Adaptive is the M1/M1b/M2
    // free-running scoring path; Trained scores against a structure frozen by
    // learn-feel-flow (training freezes the STRUCTURE -- receptive fields,
    // period ratios, the mu(region) pattern -- never the clock: omega keeps
    // adapting to whatever material is scored, so a tempo difference between
    // the trained-from material and the scored material is never misread as
    // a static lean).
    enum class FeelFlowMode { Adaptive = 0, Trained = 1 };

    static FeelFlowMode feelFlowModeFromString( const QString &, bool *ok = nullptr );
    static QString feelFlowModeToString( FeelFlowMode );

    FeelFlowMode feelFlowMode() const { return feelFlowMode_; }

    // Called ONLY by SSetFeelFlowModeAction -- direct model mutation, mirrors
    // every other setFooInternal() this track exposes to its own action
    // (e.g. setMidiOutput). Does NOT bump any content epoch: a mode change
    // affects what the NEXT bounce+analysis computes, not the audio graph,
    // so it must never look like a chain edit to the render path.
    void setFeelFlowModeInternal( FeelFlowMode mode ) { feelFlowMode_ = mode; }

    // True once learn-feel-flow has produced a frozen structure for this
    // track (whether or not the track is currently IN Trained mode).
    bool feelFlowHasTrainedStructure() const { return (bool) feelFlowTrained_; }

    // Null when feelFlowHasTrainedStructure() is false.
    const twGrooveTrainedStructure *feelFlowTrainedStructure() const
    { return feelFlowTrained_.get(); }

    // Called ONLY by SLearnFeelFlowAction (apply AND its inverse -- an
    // undo/redo of learn-feel-flow is exactly "replace the trained structure
    // with the one that was there before"). Takes ownership; null clears it.
    void setFeelFlowTrainedStructureInternal(
        std::unique_ptr<twGrooveTrainedStructure> structure );

    // A COPY of the current trained structure (null when there is none), for
    // an action to stash as its own undo state without aliasing the track's
    // live copy.
    std::unique_ptr<twGrooveTrainedStructure> copyFeelFlowTrainedStructure() const;

    // The last bounce's output path, or empty when this track has never
    // been bounced (SFeelFlowTrackBounce::bouncePath()). learn-feel-flow's
    // read source.
    std::string feelFlowBouncePath() const;

    // --- proposal 40 M3b: the metric-lab band selection -------------------
    //
    // WHICH derived metric series (SFeelFlowUiData::metrics, by id) the
    // arranger's Feel Flow band paints for this track. RUNTIME-ONLY and
    // never serialized -- a view preference for assessing the metric lab,
    // not an edit to the arrangement -- so the setter is a plain call (the
    // panel's combo, the set-feel-flow-metric verb), never an action. The
    // default "compliance" is the shipped scalar, which keeps every
    // pre-M3b gate byte-unchanged by construction; an id the current
    // snapshot does not carry falls back to compliance at PAINT time
    // (SFeelFlowUiData::metricById's doc), so a stale selection can dim
    // nothing and break nothing.
    // Since M3d the selection is a LIST -- a comma-separated spelling in
    // and out ("sigma,lean"), so the verb, the panel and describe() speak
    // one dialect; the stored string is NORMALIZED (split, trimmed, empties
    // dropped, empty list -> "compliance"). feelFlowBandMetricIds() is the
    // parsed form the painter iterates -- one band SUB-ROW per id, in list
    // order, top to bottom.
    const std::string &feelFlowBandMetricId() const { return feelFlowBandMetricId_; }
    const std::vector<std::string> &feelFlowBandMetricIds() const
    { return feelFlowBandMetricIds_; }
    void setFeelFlowBandMetricId( const std::string &idList );

    // --- the folder-sum preview (proposal 39 M3, design D3) ---------------

    /**
     * True if any child link of ours is itself an STrack — i.e. we are a
     * FOLDER lane. THE one definition: the arranger used to spell it locally in
     * sstdmixerview.cpp to decide whether a row gets a fold triangle, and the
     * overlay asks the same question, so a second copy would be two answers to
     * "is this a folder".
     */
    bool hasChildTracks() const;

    /**
     * UI fold state (fix/track-list-polish m): whether the arranger draws this
     * folder lane's children collapsed. Purely cosmetic — it changes no audio
     * and nothing in the render path reads it — but it is a per-TRACK fact,
     * so it belongs here rather than in a second, view-owned copy that would
     * have to be pruned by hand whenever a track goes away (the fold set used
     * to be exactly that: a `QSet<STrack*>` on `SStdMixerView`, kept in sync
     * only by `pruneUiState()`). Living on the object means it dies with the
     * object, needs no pruning, and — the point of this change — survives a
     * save/load round trip for free through the ordinary attribute path.
     * Written only when true, so every project saved before this exists
     * loads and re-saves byte-identically.
     */
    bool isCollapsed() const { return collapsed_; }
    void setCollapsed( bool c ) { collapsed_ = c; }

    /**
     * Fill out[0..win.width) with the SUM OF OUR DESCENDANTS' DRAWN ENVELOPES
     * over `win`, and return true; false — writing nothing — when nothing
     * contributed, so the painter draws nothing at all.
     *
     * IT IS A SUM OF ENVELOPES, NOT THE ENVELOPE OF A SUM, and that is the
     * whole design (proposal 39, "The headline finding"). The exact answer
     * exists — STrack::getPreview() already returns this folder's summed
     * output — and it is unusable here: its container branch reaches its pages
     * through requestPage(), which DEMANDS A FREEZE, and this runs from
     * paintEvent (main/timeline/CONTRACT.md inv. 1 forbids a paint path that
     * blocks). So the overlay is built from previews that already exist: it
     * over-states where children are out of phase, and it cannot see child
     * plugins, instruments or automation, which live only in frozen pages. A
     * hint about where material is — never a meter and never an oracle.
     *
     * Three rules decide what the numbers mean:
     *
     * - OUR OWN FADER IS NOWHERE IN IT. A drawn waveform describes the audio
     *   its object produces and the lane it is drawn on never scales it
     *   (the rule M2 adopted), and this overlay IS the lane. A descendant's
     *   contribution is scaled by the product of the gains of every track from
     *   its own track up to but EXCLUDING us — so a child one level down keeps
     *   its own fader, because that child is not the lane being drawn on.
     * - AUDIBILITY IS ssolo::isLaneAudible, never a local mute/solo chain.
     *   main/timeline/CONTRACT.md inv. 10 records what local copies of that
     *   rule cost the last time: a solo nested inside a folder was a no-op and
     *   the meters disagreed with the ear.
     * - THE ACCUMULATOR IS int32 AND THE CLAMP HAPPENS ONCE, at the end.
     *   preview_t is a signed char; accumulating in it wraps, and a wrap makes
     *   two loud children draw QUIETER than one — a failure that looks like a
     *   feature.
     *
     * Never blocks, never freezes, never demands: every probe is an index into
     * an array a child's preview already built.
     */
    bool collectChildSumEnvelope( const SEnvelopeWindow &win,
                                  preview_t *out ) const;

    // Path search may descend into track lanes (see SObject::isPathContainer).
    virtual bool isPathContainer() const override { return true; }
    // A track IS a lane (proposal 41 D3): it carries solo, mute, edit-group
    // membership and arm state, unlike a fragment (isPathContainer() true,
    // isLane() false).
    virtual bool isLane() const override { return true; }
    virtual bool hasDuration() const override;
    virtual length_t getDuration() const override;

    // Scoped invalidation (proposal 15): stale this track's engine chain
    // (mixers, plugin chains + inserts, rewire). Ancestors are handled by
    // SObject::invalidateRenderPath()'s walk from the project root.
    void bumpRenderChainEpoch() override;
    void bumpRenderChainEpochRange( offset_t start, offset_t end ) override;

    /**
     * We are a summing container for any child TRACK (a folder lane), so we
     * enforce the shared mute/solo audibility rule on those lanes by muting
     * their clip entries (twTrackMix::setClipMuted) — the root mixer's
     * null-the-input-plug trick cannot reach a nested track. Recurses into
     * nested folders, and range-invalidates exactly what changed. Idempotent;
     * driven top-down from SStdMixer::applyAudibility().
     */
    void applyChildTrackAudibility();

public slots:
    // Set the channel width of this track's chain. Connected to
    // SProject::channelsChanged; also the constructor's first call, which is
    // what builds the components. Creates and destroys nothing on a later
    // call — which is why a shrink is now ordinary (the old setNBusses()
    // refused one with a Q_ASSERT_X that the shipped build compiled out).
    void setChannels( int n );
    void onPluginSlotInserted( int index, SPluginSlot &slot );
    void onPluginSlotRemoved( int index, SPluginSlot &slot );
    void onPluginSlotsReordered( int fromIndex, int toIndex );
    // One of our slots changed what it produces (bypass, a parameter, a reload).
    // The slot cannot invalidate the path above itself — the chain is not an
    // SLink child of ours, so SObject::invalidateRenderPath()'s root-down walk
    // never reaches it — so it says so and WE invalidate from here, exactly as
    // onPluginSlotInserted/Removed/Reordered already do.
    void onPluginSlotAudioInvalidated();
    void onTrackMuteChanged( bool muted );
    // A slot's `param:` lane changed over [start, INT64_MAX): a plugin is
    // class-1 (stateful), so an edit at `a` can change every page after it.
    // Routed through a SIGNAL for the same reason audioInvalidated() is —
    // SPluginChain is deliberately not an SLink child, so the slot cannot
    // reach the containers above it on its own (pluginui inv. 6).
    void onPluginSlotAudioInvalidatedRange( qint64 start, qint64 end );
    // A child track (folder lane) changed its mute; we are its summing parent.
    void childTrackMuteChanged( bool muted );
    // Somewhere in our subtree a lane's solo flag flipped. Solo is GLOBAL, so
    // we cannot resolve it ourselves — we relay it upwards (subtreeSoloChanged)
    // until it reaches the root mixer, which re-applies the rule to the whole
    // tree. Connected to both a child track's soloChanged and a child folder's
    // subtreeSoloChanged, so it works at any nesting depth.
    void childTrackSoloChanged();
    void onTrackVolumeChanged( double gainDb );

public:
    // --- automation (proposal 37 P5, design D5) ----------------------------
    //
    // A track owns the `self:Volume` and `self:Muted` lanes; both are consumed
    // by twGainStage, POST-FX. Volume in TRIM is SUMMED with the fader's own
    // dB (dB sum == gain product); in READ the curve is the whole value.
    //
    // The MUTE LANE is the audio mute and it is ramped. The mute BUTTON stays
    // STRUCTURAL (the parent nulls our input plug / skips our clip entry) —
    // they are different things, and an asset capture of a lane-muted track
    // hears the lane exactly as a render does.
    virtual void onAutomationChanged( SAutomationLane &lane,
                                      offset_t start, offset_t end ) override;
    virtual void applyAutomationToEngine() override;

private:
    // Push both lanes' current snapshots into the gain stage. Null curve ==
    // the scalar path, which is what keeps an un-automated track byte-exact.
    void pushTrackAutomation();
    // Re-read every clip's `cut:Gain` envelope into its twTrackMix entry. The
    // curve lives on the WINDOW (an SCut), and a window is not allowed to know
    // its track — so this runs where every model change already funnels
    // through on the MAIN thread: bumpRenderChainEpoch[Range](), the same hook
    // refreshInstrumentFeed() uses and for the same reason.
    void refreshClipGainCurves();

// BACK INTO THE SLOTS SECTION. Everything from here to `signals:` was declared
// under `public slots:` above and MUST stay there: trackEventClipChanged is
// connected by NAME (SIGNAL/SLOT macros), so demoting it to a plain member
// makes the connect fail at runtime with a warning nobody reads and every
// event-clip edit silently stops reaching the render.
public slots:
    /** An event clip of ours changed its content or its window (3.2). */
    void trackEventClipChanged( offset_t fromClipPos );

signals:
    void nChannelsChanged( int n );
    // A lane at or below us changed its solo flag (see childTrackSoloChanged).
    void subtreeSoloChanged();
    // trackInput / monitorMode changed (proposal 21 L1b). A plan-rebuild
    // trigger, design §3: SLiveMonitor connects to it.
    void trackInputChanged();

protected:
    
private:
    void checkDurationChanged();
    /**
     * Point slot 0's processor at THIS track's feed when slot 0 is an
     * instrument, and take the feed away from every other slot (design 4.3).
     * Also pushes the project's tempo map, which is the transport half of
     * twProcessContext. Main thread only — it reaches twPlugin::prepare().
     */
    void syncInstrumentSlot();
    /**
     * Rebuild the feed's SOURCE LIST for an instrument we already wired.
     *
     * The processor holds the merge OBJECT, which is stable; what moves is
     * which children feed it — a child added, re-parented, muted, solo-excluded
     * or re-routed. STrack::eventFeed() rebuilds that list, and it must run on
     * the MAIN thread (it walks childLinks() and resolves solo over the whole
     * tree). The invalidation hooks below are exactly the main-thread points
     * every such change already passes through, and they are guarded on there
     * being an instrument at all, so a project without one pays nothing.
     */
    void refreshInstrumentFeed();
    // Wire a chain to this track: signals, our reference, the DSP component
    // provider. Used by the constructor and by adoptPluginChain().
    void connectPluginChain( SPluginChain *chain );

    SStartTimeList startTimeList_;
    SEndTimeList endTimeList_;
    STrackRendererInline *inlineRenderer_;
    int channels_;                                  // see setChannels()
    std::shared_ptr<twTrackMix> cpTrackMix_;        // ONE, channels_ wide
    // THE FADER (proposal 37 P3a). Sits between the DSP chain and the rewire,
    // so the scalar is applied POST-FX: trackmix -> chain -> gain -> rewire.
    // It used to be a scalar inside twTrackMix, i.e. PRE-FX (design F6).
    std::shared_ptr<twGainStage> cpGainStage_;      // ONE, channels_ wide
    std::shared_ptr<twRewire> cpRewire_;            // the track's ROOT component
    // Proposal 40 M1b: lazily created on the first startFeelFlowBounce().
    // Not an SObject/SLink — derived data with no independent project
    // existence, like an automation lane, and its destructor JOINS any
    // in-flight bounce (see sfeelflowbounce.h) before the rest of ~STrack
    // runs, which is what keeps the analysis job's captured SProject/
    // CaptureRevalidator pointers valid: ~SProject deletes tracks from its
    // own destructor BODY, before its revalidator_ member is torn down.
    std::unique_ptr<SFeelFlowTrackBounce> feelFlowBounce_;
    // Proposal 40 M3. Project state (unlike feelFlowBounce_ above, which is
    // session-local derived data): feelFlowMode_ default-constructs to
    // Adaptive and feelFlowTrained_ default-constructs empty, which is what
    // keeps serialize() writing NOTHING for every track that never touches
    // Feel Flow (see serialize()/readPostChildrenAttributes() in strack.cpp).
    FeelFlowMode feelFlowMode_ = FeelFlowMode::Adaptive;
    std::unique_ptr<twGrooveTrainedStructure> feelFlowTrained_;
    // Proposal 40 M3b: runtime-only view preference (see the accessor's
    // doc). Deliberately NOT serialized and NOT part of staleness. The
    // string is the normalized comma-joined spelling; the vector is its
    // parsed form (M3d), kept in step by the ONE setter.
    std::string feelFlowBandMetricId_ = "compliance";
    std::vector<std::string> feelFlowBandMetricIds_ = { "compliance" };
    SPluginChain *cpPluginChain_;  // Model object for effects inserts
    // Our REFERENCE to that chain, not a child link (a chain is not an SLink
    // child of a track — SObject::childEvent only accepts SLinks, and a chain in
    // childLinks() would be treated as a clip). It exists because an SObject
    // whose reference count reaches zero deleteLater()s itself: the loader's
    // temporary handle link is dropped in ~SProjectLoader, so without a
    // reference of our own an adopted chain would be destroyed moments after we
    // took it. Holding one for the constructor-made chain too keeps the
    // refcount — a serialized attribute — identical between a new and a loaded
    // project. Deleted by ~STrack.
    SLink *cpPluginChainRef_ = nullptr;
    std::shared_ptr<twPluginChain> cpDspChain_;     // DSP component, ONE, channels_ wide

    // The event twin of cpTrackMixers_ — ONE set per track, not one per bus:
    // events are not per bus (4.2). Held by shared_ptr because a parent's
    // feed merges it in and must not be able to outlive it in flight.
    std::shared_ptr<twEventClipSet> eventClips_;
    std::shared_ptr<twEventMerge>   eventFeed_;
    MidiRouting                     midiRouting_ = MidiRouting::Auto;
    // MIDI output (P7b). Portable NAME + 0-based channel (-1 = as authored) +
    // signed send offset in ms. Serialized only when non-default, so every
    // project written before proposal 37 re-serializes byte-identically.
    QString                         midiOutPort_;
    // Live input / monitoring (proposal 21 L1b). trackInput_ is the portable
    // selector string; liveOwnedLane_ is session state and is never written.
    QString                         trackInput_;
    MonitorMode                     monitorMode_ = MonitorMode::Auto;
    bool                            liveOwnedLane_ = false;
    // The invalidation owed to the root while live-owned (proposal 21 L3b).
    bool     haveDeferredDirty_  = false;
    offset_t deferredDirtyStart_ = 0;
    offset_t deferredDirtyEnd_   = 0;
    int                             midiOutChannel_ = -1;
    int                             midiOutOffsetMs_ = 0;
    // See setSystemRole(): immutable after construction.
    SSystemRole                     systemRole_ = SSystemRole::None;
    // Fold state (fix/track-list-polish m). See isCollapsed()/setCollapsed().
    bool                            collapsed_ = false;

    mutable length_t lastDuration_;
    mutable bool lastDurationValid_;

    virtual int serializeSelfAttributes( QTextStream &o ) override;
    
private slots:    
    void trackChildWasAdded( SLink & );
    void trackChildWasRemoved( SLink & );
    void trackChildWasMoved( offset_t newTime );
    void trackChildDurationChanged( length_t newLength );
};

#endif
