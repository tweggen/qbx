
#ifndef _SSTDMIXER_H_
#define _SSTDMIXER_H_

//#include <qptrlist.h>

#include "app/model/sobject.h"
// Complete type needed by the QPointer<STrack> members below (QPointer has to
// see the QObject base). strack.h is a leaf-ish header — qobject + sobject.
#include "app/objects/track/strack.h"

#include <QList>
#include <QPointer>

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
    virtual std::shared_ptr<twComponent> getRootComponent() override;

    virtual QWidget *getDetailEditWidget( QWidget *parent ) override;
    virtual QWidget *getInlineEditWidget( QWidget *parent ) override;
    virtual SObjectRenderer *getInlineRenderer() override;
    
    virtual int getNTracks() const;
    // The mixer holds lanes; path search and the placement service treat it
    // as a container (see SObject::isPathContainer).
    virtual bool isPathContainer() const override { return true; }
    // Generic view of the selected track (see SObject::activeLane).
    virtual SObject *activeLane() const override;
    virtual SLink *getTrackAt( int idx );

    virtual int seekTo( offset_t ) override;

    virtual length_t getDuration() const override;
    virtual bool hasDuration() const override;

    // --- track selection ------------------------------------------------
    // The selection is a SET of tracks with one distinguished PRIMARY (the
    // last one the user touched). The primary is what the Track Detail dock
    // and every "the selected track" caller sees; the set is what the arranger
    // broadcasts mute/solo/arm and the structural track operations over.
    //
    // Held as QPointers on purpose: SRemoveTrackAction keeps a removed track
    // alive for undo, but a discarded command finally deletes it — a raw
    // pointer in here would then dangle until the next click.
    STrack *getSelectedTrack() const;
    // Selection in no particular order, pruned of tracks that have died. The
    // arranger re-orders it by lane when order matters.
    QList<STrack *> getSelectedTracks() const;
    int nSelectedTracks() const;
    bool isTrackSelected( STrack *track ) const;

    // Select exactly this one track (it becomes the primary). NULL clears.
    void setSelectedTrack( STrack *track );
    // Replace the whole selection. `primary` must be a member; when it is NULL
    // the last entry of `tracks` becomes the primary.
    void setSelectedTracks( const QList<STrack *> &tracks,
                            STrack *primary = nullptr );
    // Add/remove `track` (Ctrl-click). A track that joins becomes the primary;
    // when the primary leaves, the last remaining member takes over.
    void toggleTrackSelection( STrack *track );

    // Scoped invalidation (proposal 15): the mixer's rewire is the engine's
    // synthOutput_ — its cached pages hold the summed mix and go stale on any
    // contained edit.
    void bumpRenderChainEpoch() override;
    void bumpRenderChainEpochRange( offset_t start, offset_t end ) override;

signals:
    void nBussesChanged( int n );
    void trackInserted( int newIndex, STrack &pt );

    /**
     * This signal is emitted, if a track is removed.
     * The former index of the track is given, in addition to a reference to
     * the track.
     */
    void trackRemoved( int oldIndex, STrack &pt );

    /**
     * Emitted when the track order changed in place (no track added or removed).
     * A hook for views to re-sequence their lanes; the model order is already
     * updated when this fires.
     */
    void tracksReordered();

    /**
     * Emitted when a different track becomes the PRIMARY selection (for the
     * detail panel). Always preceded by selectedTracksChanged() when the set
     * changed too, so a listener that only wants "what is highlighted" can
     * watch the set signal alone.
     */
    void selectedTrackChanged( STrack *track );

    /**
     * Emitted when the selection SET changed (members added or removed),
     * whether or not the primary moved. Carries no payload: listeners ask
     * isTrackSelected() for the track they care about.
     */
    void selectedTracksChanged();

public slots:
    /**
     * Set the number of output busses.
     * The number of output busses in addition determines
     * the number of actual physical mixers.
     */
    int setNBusses( int n );

    /**
     * Set the CHANNEL WIDTH of the master sum (proposal 36 B4).
     *
     * This is what fixes the drop the proposal names in §2 item 3: the ctor's
     * hard-coded setNBusses(1) meant the summing loop ran `bus < 1`, so every
     * track's bus 1 was built, filtered, plugin-processed — and dropped. It is
     * not fixed by making the BUS count 2; a bus was never a channel. There is
     * ONE bus mixer, N channels wide, and every track's page arrives whole.
     *
     * The width comes from SProject::channels() — read in the constructor and
     * followed through SProject::channelsChanged — so it is PERSISTED where it
     * always belonged, as one attribute on the project, rather than as a second
     * copy on the mixer that could disagree with it.
     */
    void setChannels( int n );
    int getChannels() const { return channels_; }

    /**
     * Append a track to the mixer (QObject children are append-only; use
     * reorderTrack() to position it). Emits trackInserted() with the track's
     * actual landing index.
     */
    void insertTrack( STrack &track );

    /**
     * Move the track at fromIndex to toIndex, re-sequencing the others, then
     * rewire the bus inputs (assigned by index) and emit tracksReordered().
     */
    void reorderTrack( int fromIndex, int toIndex );

    /**
     * Announce that the track tree changed somewhere (e.g. a reparent into a
     * folder, or a reorder inside one) so views rebuild. Emitted as
     * tracksReordered() since the effect on a view is the same: re-walk the tree.
     * Used by the tree-editing actions after they finish mutating.
     */
    void notifyTreeChanged();

    /**
     * Remove the specified track.
     */
    int removeTrack( int trackIndex );

    /**
     * Remove the specified track. The first occurance of the given track
     * will be removed.
     */
    int removeTrack( SLink &track );

    /**
     * True if at least one lane ANYWHERE in the project (nested lanes included)
     * has its solo flag set — i.e. solo is in force. Public because audibility
     * is not knowable from a single track; the shared rule that consumes this
     * lives in app/model/ssolorules.h and is what the meters apply too.
     */
    bool anyTrackSoloed() const;

    /**
     * Re-apply the mute/solo audibility rule to the whole lane tree: our own
     * bus wiring plus every folder track's per-lane clip mutes. Idempotent.
     */
    void applyAudibility();

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
    std::vector<std::shared_ptr<twMixer> > cpMixers_;
    std::shared_ptr<twRewire> cpRewire_;
    int nBusses_;
    int channels_ = 1;      // see setChannels()

    mutable length_t lastDuration_;
    mutable bool lastDurationValid_;

    // See the selection block above. selectedTracks_ always contains
    // selectedTrack_ while the latter is non-null.
    QPointer<STrack> selectedTrack_;
    QList<QPointer<STrack> > selectedTracks_;
};

#endif
