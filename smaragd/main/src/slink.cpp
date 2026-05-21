
#include "sobject.h"
#include "slink.h"

int SLink::serializeSelfAttributes( QTextStream &o )
{
    o << " objectId='" << (unsigned long)(&object_) << "'"
      << " hasStartTime='" << hasStartTime() << "'";
    if( hasStartTime() ) {
        o << " startTime='" << (double)getStartTime() << "'";
    }
    return 0;
}

int SLink::readAttributes( QDomElement &element )
{
    QString data;

    data = element.attribute( "startTime", "0" );
    setStartTime( data.toULongLong() );
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

twComponent &SLink::getRootComponent() const
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
    qWarning( "SLink dtor.\n" );
    object_.removeRef();
}

SLink::SLink( SObject &sobject, SObject *parent /*=0*/ )
    : QObject( parent ),      
      startTime_( 0 ),
      object_( sobject )
{
    object_.addRef();
}

SLink::SLink( const SLink &other )
    : QObject( other.parent() ),
      startTime_( other.getStartTime() ),
      object_( other.getSObject() )
{
    object_.addRef();
}
