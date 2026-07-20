
#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"

#include <cstdint>

int SLink::serializeSelfAttributes( QTextStream &o )
{
    o << " objectId='" << reinterpret_cast<std::uintptr_t>(&object_) << "'"
      << " hasStartTime='" << hasStartTime() << "'";
    if( hasStartTime() ) {
        Fraction startTimeFrac(startTime_, 1);
        o << " startTime='" << QString::fromStdString( startTimeFrac.toString() ) << "'";
    }
    return 0;
}

int SLink::readAttributes( QDomElement &element )
{
    QString data;

    data = element.attribute( "startTime", "0" );
    Fraction startTimeFrac = parseFractionOrDouble( data.toStdString() );
    offset_t startTimeOffset = (offset_t)startTimeFrac.toDouble();
    setStartTime( startTimeOffset );
    return 0;
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
