
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

    // A *seekable* component rendering this track's summed output, for snapshot
    // capture (asset previews / proposal 07). Unlike getRootComponent() (the
    // output rewire, whose streaming latches carry playback state and can't seek
    // to zero), the per-bus twTrackMix seeks cleanly and re-seeks its children
    // each buffer. Bus 0 is the output for the common single-bus track. NULL if
    // unbuilt.
    std::shared_ptr<twComponent> getCaptureComponent() const;

    virtual int readPreChildrenAttributes( QDomElement &element ) override;

    virtual QWidget *getDetailEditWidget( QWidget *parent ) override;
    virtual QWidget *getInlineEditWidget( QWidget *parent ) override;
    virtual SObjectRenderer *getInlineRenderer() override;
    
    virtual SLink *getTopMostSLinkAt( offset_t ) const;
    int getNBusses() const { return nBusses_; }
    SPluginChain *getPluginChain() const { return cpPluginChain_; }
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


public slots:
    void setNBusses( int n );
    void onPluginSlotInserted( int index, SPluginSlot &slot );
    void onPluginSlotRemoved( int index, SPluginSlot &slot );
    void onPluginSlotsReordered();
    void onTrackMuteChanged( bool muted );
    void onTrackVolumeChanged( double gainDb );

signals:
    void nChannelsChanged( int n );

protected:
    
private:
    void checkDurationChanged();

    SStartTimeList startTimeList_;
    SEndTimeList endTimeList_;
    STrackRendererInline *inlineRenderer_;
    int nBusses_;
    std::vector<std::shared_ptr<twTrackMix> > cpTrackMixers_;
    std::shared_ptr<twRewire> cpRewire_;
    SPluginChain *cpPluginChain_;  // Model object for effects inserts
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
