
#ifndef _STRACK_H_
#define _STRACK_H_

#include <qobject.h>
#include <qlist.h>
#include "sobject.h"

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

    virtual twComponent &getRootComponent();

    // A *seekable* component rendering this track's summed output, for snapshot
    // capture (asset previews / proposal 07). Unlike getRootComponent() (the
    // output rewire, whose streaming latches carry playback state and can't seek
    // to zero), the per-bus twTrackMix seeks cleanly and re-seeks its children
    // each buffer. Bus 0 is the output for the common single-bus track. NULL if
    // unbuilt.
    twComponent *getCaptureComponent() const;

    virtual int readPreChildrenAttributes( QDomElement &element );

    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();
    
    virtual SLink *getTopMostSLinkAt( offset_t ) const;
    int getNBusses() const { return nBusses_; }
    SPluginChain *getPluginChain() const { return cpPluginChain_; }
    virtual int seekTo( offset_t ofs );

    virtual bool hasDuration() const;
    virtual length_t getDuration() const;

    // Phase 5e: Page cache implementation
    // Composite previews from all visible children into page cache
    virtual void recomputePreview(CapturePageData& page) override;

public slots:
    void setNBusses( int n );
    void onPluginSlotInserted( int index, SPluginSlot &slot );
    void onPluginSlotRemoved( int index, SPluginSlot &slot );
    void onPluginSlotsReordered();

signals:
    void nChannelsChanged( int n );

protected:
    
private:
    void checkDurationChanged();

    SStartTimeList startTimeList_;
    SEndTimeList endTimeList_;
    STrackRendererInline *inlineRenderer_;
    int nBusses_;
    twTrackMix **cpTrackMixers_;
    twRewire *cpRewire_;
    SPluginChain *cpPluginChain_;  // Model object for effects inserts
    twPluginChain **cpPluginChains_;  // DSP components (one per bus)
    
    mutable length_t lastDuration_;
    mutable bool lastDurationValid_;

    virtual int serializeSelfAttributes( QTextStream &o );
    
private slots:    
    void trackChildWasAdded( SLink & );
    void trackChildWasRemoved( SLink & );
    void trackChildWasMoved( offset_t newTime );
    void trackChildDurationChanged( length_t newLength );
};

#endif
