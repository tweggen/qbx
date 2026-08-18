
#ifndef _STRACK_H_
#define _STRACK_H_

#include <qobject.h>
#include <qlist.h>
#include <memory>
#include "app/model/sobject.h"
#include "tw/events/tweventclipset.h"
#include "tw/events/tweventmerge.h"

class twComponent;
class STrack;
class SObjectRenderer;
class STrackRendererInline;
class SPluginChain;
class SPluginSlot;
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

    // Path search may descend into track lanes (see SObject::isPathContainer).
    virtual bool isPathContainer() const override { return true; }
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
