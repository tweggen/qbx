
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

    virtual int readPreChildrenAttributes( QDomElement &element ) override;

    virtual QWidget *getDetailEditWidget( QWidget *parent ) override;
    virtual QWidget *getInlineEditWidget( QWidget *parent ) override;
    virtual SObjectRenderer *getInlineRenderer() override;
    
    virtual SLink *getTopMostSLinkAt( offset_t ) const;
    int getNBusses() const { return nBusses_; }

    // TEST HOOK (proposal 36 M1). Every entry into setNBusses() is counted,
    // process-wide, whether or not it changes anything.
    //
    // It exists because M1's central claim is a NEGATIVE one — the project's new
    // channel count reaches no bus count — and a negative is exactly what code
    // review is worst at: the next person to add a connection between
    // SProject::channelsChanged and a track would pass every audio assertion in
    // the suite and only discover the shrink Q_ASSERT_X on an undo. Counting the
    // calls turns "we did not wire it" into something a test can fail on.
    // Retire it when B4 makes bus width a real consequence of project width.
    static long setNBussesCallCount();

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

    // Adopt a plugin chain loaded from a project file (proposal 08 M4): drop the
    // empty one the constructor made, take over the loaded one, reconnect its
    // signals and rebuild the per-bus DSP chains from its slots. Called from the
    // loader's deferred-resolve pass, never during normal editing.
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
    void setNBusses( int n );
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
    // A child track (folder lane) changed its mute; we are its summing parent.
    void childTrackMuteChanged( bool muted );
    // Somewhere in our subtree a lane's solo flag flipped. Solo is GLOBAL, so
    // we cannot resolve it ourselves — we relay it upwards (subtreeSoloChanged)
    // until it reaches the root mixer, which re-applies the rule to the whole
    // tree. Connected to both a child track's soloChanged and a child folder's
    // subtreeSoloChanged, so it works at any nesting depth.
    void childTrackSoloChanged();
    void onTrackVolumeChanged( double gainDb );
    /** An event clip of ours changed its content or its window (3.2). */
    void trackEventClipChanged( offset_t fromClipPos );

signals:
    void nChannelsChanged( int n );
    // A lane at or below us changed its solo flag (see childTrackSoloChanged).
    void subtreeSoloChanged();

protected:
    
private:
    void checkDurationChanged();
    // Wire a chain to this track: signals, our reference, the DSP component
    // provider. Used by the constructor and by adoptPluginChain().
    void connectPluginChain( SPluginChain *chain );

    SStartTimeList startTimeList_;
    SEndTimeList endTimeList_;
    STrackRendererInline *inlineRenderer_;
    int nBusses_;
    std::vector<std::shared_ptr<twTrackMix> > cpTrackMixers_;
    std::shared_ptr<twRewire> cpRewire_;
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
    std::vector<std::shared_ptr<twPluginChain> > cpPluginChains_;  // DSP components (one per bus)

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
