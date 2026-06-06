
#ifndef _SSTDMIXER_H_
#define _SSTDMIXER_H_

//#include <qptrlist.h>

#include "sobject.h"

class twComponent;
class twMixer;
class twRewire;
class STrack;
class SObjectRenderer;
class SProjectLoader;

class SStdMixer;

/**
 * This is the mixer object below the standard arranger.
 * It has a variable number of tracks, each of them is an
 * SLink to an STrack.
 *
 * The mixer object creates a hierarchy among the track objects.
 *
 * The mixer's widget renders the single tracks in vertical order,
 * each track having an integer specifying its height in multiples
 * of a single track height. 
 */
class SStdMixer
    : public SObject
{
    Q_OBJECT
public:
    SStdMixer( SProject *project );
    virtual ~SStdMixer();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader, 
					     QDomElement &element, 
					     SObject *parent );

    /// For SObject
    virtual twComponent &getRootComponent();

    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();
    
    virtual int getNTracks() const;
    virtual SLink *getTrackAt( int idx );

    virtual int seekTo( offset_t );

    virtual length_t getDuration() const;
    virtual bool hasDuration() const;

signals:
    void nBussesChanged( int n );
    void trackInserted( int newIndex, STrack &pt );

    /**
     * This signal is emitted, if a track is removed.
     * The former index of the track is given, in addition to a reference to
     * the track.
     */
    void trackRemoved( int oldIndex, STrack &pt );
    
public slots:
    /**
     * Set the number of output busses.
     * The number of output busses in addition determines
     * the number of actual physical mixers.
     */
    int setNBusses( int n );

    /**
     * Insert another track at the given position. 
     * Tracks with numerically greater indices appear after the new track.
     * A special index of -1 is identically to specifying the highest available
     * track number.
     */
    void insertTrack( int newIndex, STrack &track );

    /**
     * Remove the specified track.
     */
    int removeTrack( int trackIndex );

    /**
     * Remove the specified track. The first occurance of the given track
     * will be removed.
     */
    int removeTrack( SLink &track );

protected:
private slots:
    void mixerUpdateTrackRemoved( int, STrack & );
    void mixerUpdateTrackAdded( int, STrack & );
    void mixerChildDurationChanged( length_t );
    // A track's mute or solo flag changed: re-evaluate routing for all tracks
    // (solo on any track silences the others).
    void trackMuteSoloChanged();

private:
    void checkDurationChanged();
    void reconnectTracksToMixer();
    bool anyTrackSoloed() const;
    twMixer **cpMixers_;
    twRewire *cpRewire_;
    int nBusses_;
    
    mutable length_t lastDuration_;
    mutable bool lastDurationValid_;
};

#endif
