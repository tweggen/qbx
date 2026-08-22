
#ifndef _SLINK_H_
#define _SLINK_H_

#include <qobject.h>
#include "tw/graph/tw303aenv.h"
#include "app/model/sobject.h"
#include "tw/core/twfraction.h"

class QWidget;
class twComponent;
class QTextStream;

/**
 * SLink wraps an SObject and adds timing information (startTime) to place content
 * into a timeline. SLink instances represent references to resources (whether
 * loaded from files via SCut, or other SObjects) positioned at specific times
 * within a parent container (typically an STrack).
 *
 * IMPORTANT: the parent must never reach the QObject constructor — that fires
 * the parent's childEvent() while this object is still a plain QObject (its
 * virtuals not callable; SObject::childEvent rejects such children). Both
 * constructors therefore build with QObject(NULL) and attach via setParent()
 * as their LAST step; external call sites may either pass the parent in or
 * setParent() afterwards.
 */
class SLink
    : public QObject 
{
    Q_OBJECT
public:
    /**
     * How this placement's position is ANCHORED (proposal 37 D2).
     *
     *  Time  — `startTime` (timeline frames) is the authority. A tempo change
     *          leaves the clip where it is. Audio's default, and REAPER's.
     *  Beats — `startTicks` (exact rational musical ticks) is the authority
     *          and `startTime` is DERIVED through the project's tempo map. A
     *          tempo change moves the clip so it stays at the same bar. The
     *          default for an EVENT clip: recorded MIDI that does not follow a
     *          tempo change is a defect users hit in the first hour.
     *
     * The ticks are exact and are never re-derived from frames by `set-tempo`,
     * so repeated tempo edits cannot drift (a frames-only rescale loses a
     * frame per edit).
     */
    enum class Timebase { Time = 0, Beats = 1 };

    SLink( SObject &sobject, SObject *parent=0 );
    SLink( const SLink & );
    virtual ~SLink();

    SObject &getSObject() const
        { return object_; }

    /** The timebase a link over `obj` is born with (Beats for Event content). */
    static Timebase defaultTimebaseFor( const SObject &obj );

    Timebase getTimebase() const { return timebase_; }
    /**
     * Switch the anchor. The CURRENT position is preserved: switching to Beats
     * converts today's `startTime` to ticks once; switching to Time simply
     * stops re-deriving. Never moves the clip by itself.
     */
    void setTimebase( Timebase );

    /**
     * The musical anchor, exact. Meaningful only for a Beats link (a Time link
     * keeps it in step so a later switch has something to start from).
     */
    const Fraction &getStartTicks() const { return startTicks_; }
    /** Set the musical anchor and DERIVE startTime from it (Beats links). */
    void setStartTicks( const Fraction &ticks );

    /**
     * Proposal 41 D6: the channel remap THIS PLACEMENT applies to whatever
     * residual event feed its content exports (SObject::resolveEventFeed()).
     * -1 (default) = as-authored; 0..15 rewrites every channel-carrying
     * exported event. Lives HERE, not on the content, because D2 shares ONE
     * content object (an SCut, say) across every placement of an asset --
     * "edit any and all change" is exactly right for the content's OWN
     * window, but a channel remap is the one thing D6 explicitly wants
     * to differ PER PLACEMENT (a fragment authored on channel 10 placed on
     * two tracks whose instruments want different channels). Applied by the
     * caller that already holds the SLink (STrack::trackChildWasAdded's
     * resolveFn), never inside the content's own resolveEventFeed().
     */
    int getEventChannelOverride() const { return eventChannelOverride_; }
    void setEventChannelOverride( int channel );
    /**
     * Re-derive `startTime` from `startTicks` at the project's CURRENT tempo.
     * Called by `set-tempo` for every Beats link in the project; a no-op for a
     * Time link. Returns true when the derived frame position actually moved.
     */
    bool rederiveStartTime();

    virtual int serialize( QTextStream & );
    virtual int serializeSelfAttributes( QTextStream & );
    virtual int readAttributes( QDomElement & e );

    virtual std::shared_ptr<twComponent> getRootComponent() const;
    virtual QWidget *getDetailEditWidget( QWidget *parent = NULL );
    virtual QWidget *getInlineEditWidget( QWidget *parent = NULL );
    
    virtual bool hasStartTime() const;
    virtual offset_t getStartTime() const;

    virtual int seekTo( offset_t );
    virtual bool isEmpty() const;

    /**
     * implement comparison operators regarding start time.
     * Note that we do not check if start time was defined.
     * If you use that operator, you have to check it for yourselves.
     */
    int operator < ( SLink &other ) {
        return startTime_<other.startTime_; }
    int operator == ( SLink &other )  {
        return startTime_<other.startTime_; }

signals:
    /**
     * The link was moved in time.
     */
    void startTimeChanged( offset_t newTime );

public slots:
    /**
     * Set a different start time.
     */
    void setStartTime( offset_t );

protected:
private:
    // ticks <-> frames through the project's tempo map, which is the ONE
    // converter (events/CONTRACT inv. 4). Null project (a link built before
    // parenting, or during teardown) degrades to leaving the value alone.
    void syncTicksFromTime_();

    offset_t startTime_;
    // The musical anchor. Kept in step for every link so `set-link-timebase`
    // never has to invent one; AUTHORITATIVE only while timebase_ == Beats.
    Fraction startTicks_;
    Timebase timebase_;
    SObject &object_;
    // Proposal 41 D6. -1 = as-authored (see getEventChannelOverride() above).
    int eventChannelOverride_ = -1;
};

#endif
