#include "app/model/sunusedclips.h"

#include "app/model/sclipwindow.h"
#include "app/model/sexternfile.h"
#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"

#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace {

// EVERYTHING SOME LINK REACHES, below `obj`. Recorded per link, the way
// srecfiles' walk does it, so a take three levels down inside a wrapped
// column counts as placed exactly like a plain clip on a lane.
void collectReached_( SObject *obj, QSet<const SObject *> &out )
{
    if( !obj ) return;
    for( SLink *lk : obj->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child || out.contains( child ) ) continue;
        out.insert( child );
        collectReached_( child, out );
    }
}

// WHAT IS PLACED: everything the master, an arrangement or a registered asset
// body reaches. Asset bodies are walked as well as the roots, unlike
// srecfiles::scanUnused: a clip inside an asset that is not placed right now
// is still held by something the user named, so it is not unused. (At load
// such a clip has two references, the asset's link and the loader's handle,
// so it is never pinned. Walking the assets keeps the answer right without
// relying on that.)
QSet<const SObject *> placed_( SProject *project )
{
    QSet<const SObject *> out;
    // check_root_resolution: master -- enumerates EVERY root -- the master here, then the arrangements and asset bodies below
    if( SObject *master = project->masterRoot() ) {
        out.insert( master );
        collectReached_( master, out );
    }
    for( SObject *arr : project->arrangements() ) {
        if( !arr ) continue;
        out.insert( arr );
        collectReached_( arr, out );
    }
    const QHash<QString, SObject *> &assets = project->assets();
    for( auto it = assets.cbegin(); it != assets.cend(); ++it ) {
        if( !it.value() ) continue;
        out.insert( it.value() );
        collectReached_( it.value(), out );
    }
    return out;
}

} // namespace

namespace sunusedclips {

QString Entry::label( int sampleRate ) const
{
    const double rate = sampleRate > 0 ? double( sampleRate ) : 48000.0;
    // C-locale formatting on purpose: QString::number does not follow the
    // process locale, so the label reads the same on every machine.
    const QString from = QString::number( double( start ) / rate, 'f', 3 );
    const QString len  = QString::number( double( length ) / rate, 'f', 3 );
    const QString what = contentFile.isEmpty()
        ? QStringLiteral( "(%1)" ).arg( contentKind )
        : QFileInfo( contentFile ).fileName();
    return QStringLiteral( "%1, %2 s from %3 s" ).arg( what, len, from );
}

QList<Entry> list( SProject *project )
{
    QList<Entry> out;
    if( !project ) return out;

    const QList<SObject *> pinned = project->loadedOrphans();
    if( pinned.isEmpty() ) return out;
    const QSet<const SObject *> placed = placed_( project );

    for( int i = 0; i < pinned.size(); ++i ) {
        SObject *clip = pinned.at( i );
        if( !clip || placed.contains( clip ) ) continue;
        Entry e;
        e.clip    = clip;
        e.filePos = i;
        if( SClipWindow *w = SClipWindow::of( clip ) ) {
            e.start  = w->startOffset();
            e.length = w->duration();
            SObject &content = w->windowContent();
            if( SExternFile *f = qobject_cast<SExternFile *>( &content ) )
                e.contentFile = f->getFileName();
            else
                e.contentKind = QString::fromLatin1(
                    content.metaObject()->className() );
        } else {
            e.contentKind = QString::fromLatin1( clip->metaObject()->className() );
        }
        out.append( e );
    }

    std::stable_sort( out.begin(), out.end(),
                      []( const Entry &a, const Entry &b ) {
        if( a.contentFile != b.contentFile ) return a.contentFile < b.contentFile;
        if( a.contentKind != b.contentKind ) return a.contentKind < b.contentKind;
        if( a.start != b.start )             return a.start < b.start;
        if( a.length != b.length )           return a.length < b.length;
        return a.filePos < b.filePos;
    } );
    return out;
}

QString describe( SProject *project )
{
    const QList<Entry> l = list( project );
    const int rate = project ? project->getSRate() : 0;
    QString d = QStringLiteral( "count=%1" ).arg( l.size() );
    for( int i = 0; i < l.size(); ++i )
        d += QStringLiteral( "|%1:%2" ).arg( i ).arg( l.at( i ).label( rate ) );
    return d;
}

} // namespace sunusedclips
