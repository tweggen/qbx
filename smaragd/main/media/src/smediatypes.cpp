#include "app/media/smediatypes.h"

#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace smedia {

const QStringList &suffixesFor( int categoryMask )
{
    // suffixesFor() returns a REFERENCE (the declared signature), so a
    // combined mask needs somewhere stable to live. One tiny cache, guarded:
    // the filter is read from the main thread and captured into a walk
    // request, and gate 4 will read it from a network reply handler.
    static QMutex                    lock;
    static QHash<int, QStringList>   cache;
    static const QStringList         empty;

    if( categoryMask == 0 ) return empty;

    QMutexLocker guard( &lock );
    auto it = cache.constFind( categoryMask );
    if( it != cache.constEnd() ) return it.value();

    QStringList combined;
    if( categoryMask & Audio ) combined += kAudio;
    if( categoryMask & Midi )  combined += kMidi;
    combined.removeDuplicates();
    return *cache.insert( categoryMask, combined );
}

QString categoryMaskToString( int categoryMask )
{
    QStringList parts;
    if( categoryMask & Audio ) parts << QStringLiteral( "audio" );
    if( categoryMask & Midi )  parts << QStringLiteral( "midi" );
    return parts.join( QLatin1Char( ',' ) );
}

int categoryMaskFromString( const QString &spelling )
{
    int mask = 0;
    const QStringList parts =
        spelling.split( QLatin1Char( ',' ), Qt::SkipEmptyParts );
    for( const QString &p : parts ) {
        const QString t = p.trimmed().toLower();
        if( t == QLatin1String( "audio" ) ) mask |= Audio;
        else if( t == QLatin1String( "midi" ) ) mask |= Midi;
    }
    return mask;
}

bool suffixAccepted( const QString &fileName, const QStringList &suffixes )
{
    if( suffixes.isEmpty() ) return true;   // no filter, never "nothing"
    const QString suffix = QFileInfo( fileName ).suffix();
    if( suffix.isEmpty() ) return false;
    for( const QString &s : suffixes ) {
        if( suffix.compare( s, Qt::CaseInsensitive ) == 0 ) return true;
    }
    return false;
}

} // namespace smedia
