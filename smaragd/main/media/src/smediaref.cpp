#include "app/media/smediaref.h"

#include <QMetaType>

namespace {
const QLatin1String kScheme( "smedia://" );
}

QString SMediaRef::toUri() const
{
    // Plain concatenation, not QUrl — see the header. Do NOT strip a leading
    // '/' from the path: for the "local" source on POSIX, path IS the
    // absolute filesystem path ("/home/…"), and that leading slash is part
    // of the identity, not decoration. A prior version of this function
    // stripped it "to avoid doubling up against the separator we add here",
    // which is backwards — the separator and the path's own leading slash
    // are two DIFFERENT characters, and stripping one loses information
    // fromUri() cannot get back (it turns "/home/x" into "home/x", a
    // RELATIVE path). The resulting "smedia://local//home/x" double slash is
    // harmless: fromUri() below splits at the FIRST '/' only, so the second
    // one is simply the first character of the recovered path — exactly the
    // "scheme://authority//path" shape file: URIs already use for the same
    // reason.
    return QString( kScheme ) + sourceId + QLatin1Char( '/' ) + path;
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
