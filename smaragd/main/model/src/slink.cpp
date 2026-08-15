
#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"

#include <QDebug>
#include <cstdint>

int SLink::serializeSelfAttributes( QTextStream &o )
{
    o << " objectId='" << reinterpret_cast<std::uintptr_t>(&object_) << "'"
      << " hasStartTime='" << hasStartTime() << "'";
    if( hasStartTime() ) {
        Fraction startTimeFrac(startTime_, 1);
        o << " startTime='" << QString::fromStdString( startTimeFrac.toString() ) << "'";
    }
    // Written only when it is NOT the default for this content kind, so every
    // audio project written before proposal 36 re-serializes byte-identically
    // (persistence invariant 4) and an event clip needs no attribute either.
    if( timebase_ != defaultTimebaseFor( object_ ) ) {
        o << " timebase='"
          << ( timebase_ == Timebase::Beats ? "beats" : "time" ) << "'";
    }
    // The AUTHORITY for a beats link (D2). startTime above stays written for
    // it too: it is what an older build — and every reader that knows nothing
    // about ticks — needs, and it is exactly the derived value.
    if( timebase_ == Timebase::Beats ) {
        o << " startTicks='"
          << QString::fromStdString( startTicks_.toString() ) << "'";
    }
    return 0;
}

int SLink::readAttributes( QDomElement &element )
{
    QString data;

    const QString tb = element.attribute( "timebase" );
    if( !tb.isEmpty() ) {
        timebase_ = ( tb.compare( "beats", Qt::CaseInsensitive ) == 0 )
                      ? Timebase::Beats : Timebase::Time;
    }

    data = element.attribute( "startTime", "0" );
    Fraction startTimeFrac = parseFractionOrDouble( data.toStdString() );
    offset_t startTimeOffset = (offset_t)startTimeFrac.toDouble();
    setStartTime( startTimeOffset );

    // The exact anchor WINS over the derived frame position when the file
    // carries one: the frames were written at whatever tempo was in force,
    // and the map may have been rewritten since (or rounded differently).
    const QString ticks = element.attribute( "startTicks" );
    if( !ticks.isEmpty() ) {
        startTicks_ = parseFractionOrDouble( ticks.toStdString() );
        if( timebase_ == Timebase::Beats ) rederiveStartTime();
    }
    return 0;
}

SLink::Timebase SLink::defaultTimebaseFor( const SObject &obj )
{
    // REAPER's defaults: audio is pinned to time, MIDI follows the beat.
    return ( obj.contentKind() == SContentKind::Event ) ? Timebase::Beats
                                                        : Timebase::Time;
}

void SLink::setTimebase( Timebase tb )
{
    if( tb == timebase_ ) return;
    timebase_ = tb;
    // Switching TO beats freezes today's frame position as the musical one;
    // switching away just stops re-deriving. Either way the clip does not move.
    syncTicksFromTime_();
}

void SLink::syncTicksFromTime_()
{
    SProject *project = object_.getProjectSafe();
    if( !project ) return;
    startTicks_ = project->tempoMap()
                      .framesToTicks( (int64_t) startTime_, project->getSRate() )
                      .ticks();
}

void SLink::setStartTicks( const Fraction &ticks )
{
    startTicks_ = ticks;
    rederiveStartTime();
}

bool SLink::rederiveStartTime()
{
    if( timebase_ != Timebase::Beats ) return false;
    SProject *project = object_.getProjectSafe();
    if( !project ) return false;
    const offset_t derived = (offset_t)
        project->tempoMap()
            .ticksToFrames( TickPos( startTicks_ ), project->getSRate() )
            .floorToInt();
    if( derived == startTime_ ) return false;
    startTime_ = derived;
    emit startTimeChanged( startTime_ );
    return true;
}

int SLink::serialize( QTextStream &o )
{
    int res;
    o << "<SLink";
    res = serializeSelfAttributes( o );
    if( res<0 ) return res;
    o  << ">\n";

#if 0
    const QObjectList *children = this->children();
    if( !children ) return -1;
    QObjectListIt it( *children );            
    SObject *so;
    // FIXME: Use the start and the endtime list.
    while ( (so=(SLink *)it.current()) != 0 ) { 
        ++it;
        int res = lk->serialize( so );
        if( res<0 ) break;
    }
#endif

    o << "</SLink>\n";
    return 0;
}

std::shared_ptr<twComponent> SLink::getRootComponent() const
{
    return getSObject().getRootComponent();
}

int SLink::seekTo( offset_t ofs )
{
    return getSObject().seekTo( ofs );
}

bool SLink::isEmpty() const
{
    return getSObject().isEmpty();
}

offset_t SLink::getStartTime() const
{
    return startTime_;
}

QWidget *SLink::getDetailEditWidget( QWidget *parent )
{
    return getSObject().getDetailEditWidget( parent );
}

QWidget *SLink::getInlineEditWidget( QWidget *parent )
{
    return getSObject().getInlineEditWidget( parent );
}

bool SLink::hasStartTime() const
{
    return true;
}


void SLink::setStartTime( offset_t newStartTime ) 
{
    offset_t old = startTime_;
    bool changed = old!=newStartTime;
    startTime_ = newStartTime;
    // ONE conversion, here, at the moment the caller states a frame position
    // (D2: move-clip / duplicate-clip / place-* convert once and store ticks).
    // Every later tempo edit re-derives frames FROM the ticks, so nothing
    // accumulates rounding.
    syncTicksFromTime_();
    if( changed ) emit startTimeChanged( newStartTime );    
}


SLink::~SLink()
{
    // Detach from the parent NOW, while this object is still a fully-typed, live
    // SLink. If we don't, QObject::~QObject() fires the ChildRemoved event only
    // AFTER ~SLink has run — i.e. with the SLink vtable already torn down — so
    // the slots it reaches (SObject::childObjectRemoved →
    // STrack::trackChildWasRemoved) would call our virtual methods
    // (hasStartTime(), getRootComponent(), getStartTime(), …) on a
    // half-destroyed object. That is undefined behaviour and was observed as a
    // hang (a virtual dispatch through the being-destroyed vtable). Detaching
    // here makes the notification happen while every virtual method — and the
    // referenced SObject (removeRef runs after this) — is still valid. This
    // mirrors the construction-side rule documented in slink.h.
    setParent( nullptr );
    object_.removeRef();
}

SLink::SLink( SObject &sobject, SObject *parent /*=0*/ )
    : QObject( NULL ),
      startTime_( 0 ),
      startTicks_( 0 ),
      timebase_( defaultTimebaseFor( sobject ) ),
      object_( sobject )
{
    object_.addRef();
    // Attach only after construction (slink.h rule): a parent passed to the
    // QObject ctor fires the parent's childEvent while this object is still a
    // plain QObject — its virtuals are not callable and SObject::childEvent
    // now rejects such children. Doing the setParent here, last, keeps the
    // one-argument-with-parent call sites correct.
    if( parent ) {
        setParent( parent );
    }
}

SLink::SLink( const SLink &other )
    : QObject( NULL ),
      startTime_( other.getStartTime() ),
      startTicks_( other.getStartTicks() ),
      timebase_( other.getTimebase() ),
      object_( other.getSObject() )
{
    object_.addRef();
    // Attach only after construction (slink.h rule): a parent passed to the
    // QObject ctor fires the parent's childEvent while this object is still a
    // plain QObject, and SObject::childEvent now rejects such children.
    if( other.parent() ) {
        setParent( other.parent() );
    }
}
