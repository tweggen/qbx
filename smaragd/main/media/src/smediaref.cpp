#include "app/media/smediaref.h"

#include <QMetaType>

namespace {
const QLatin1String kScheme( "smedia://" );
}

QString SMediaRef::toUri() const
{
    // Plain concatenation, not QUrl — see the header. A leading '/' on the
    // path would double up against the separator we add here.
    QString p = path;
    while( p.startsWith( QLatin1Char( '/' ) ) ) p.remove( 0, 1 );
    return QString( kScheme ) + sourceId + QLatin1Char( '/' ) + p;
}

SMediaRef SMediaRef::fromUri( const QString &uri )
{
    SMediaRef ref;
    if( !uri.startsWith( kScheme ) ) return ref;   // invalid: empty sourceId

    const QString rest  = uri.mid( QString( kScheme ).size() );
    const int     slash = rest.indexOf( QLatin1Char( '/' ) );
    if( slash < 0 ) {
        ref.sourceId = rest;                       // "smedia://local"
        return ref;
    }
    ref.sourceId = rest.left( slash );
    ref.path     = rest.mid( slash + 1 );
    return ref;
}

namespace smedia {

void registerMediaMetaTypes()
{
    static const bool once = [] {
        qRegisterMetaType<SMediaRef>( "SMediaRef" );
        qRegisterMetaType<SMediaEntry>( "SMediaEntry" );
        qRegisterMetaType<QVector<SMediaEntry>>( "QVector<SMediaEntry>" );
        return true;
    }();
    Q_UNUSED( once );
}

} // namespace smedia
