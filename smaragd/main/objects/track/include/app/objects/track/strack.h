
#ifndef _STRACK_H_
#define _STRACK_H_

#include <qobject.h>
#include <qlist.h>
#include "app/model/sobject.h"

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
