#include "app/model/srecordingfiles.h"

#include "app/model/sexternfile.h"
#include "app/model/sproject.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include "app/model/sclipwindow.h"
#include "app/model/slink.h"

namespace {

/**
 * EVERY CONTENT OBJECT SOME CLIP WINDOWS, anywhere in the project.
 *
 * Recorded per link rather than per lane, because a take column's takes are
 * links too: a window's CONTENT is what an extern file is reached as, so both
 * the object a link points at and that object's content are recorded. A take
 * three levels down inside a wrapped column is found the same way as a plain
 * clip on a lane.
 */
void collectPlaced_( SObject *obj, QSet<const SObject *> &out )
{
    if( !obj ) return;
    for( SLink *lk : obj->childLinks() ) {
        if( !lk ) continue;
        SObject *child = &lk->getSObject();
        if( !child ) continue;
        out.insert( child );
        if( SClipWindow *w = SClipWindow::of( child ) )
            out.insert( &w->windowContent() );
        collectPlaced_( child, out );
    }
}

} // namespace

namespace srecfiles {

static void collectPlacedContent( SProject *project,
                                  QSet<const SObject *> &out )
{
    if( !project ) return;
    if( SObject *master = project->getRootComponent() )
        collectPlaced_( master, out );
    for( SObject *arr : project->arrangements() )
        if( arr ) collectPlaced_( arr, out );
}

} // namespace srecfiles

namespace srecfiles {

QString captureName( const QString &stamp, int trackIndex )
{
    return QStringLiteral( "%1_track%2.wav" ).arg( stamp ).arg( trackIndex );
}

bool isCaptureName( const QString &baseName )
{
    // ANCHORED AND STRICT (see the header). The stamp is the recorder's own
    // `yyyyMMdd_HHmmss_zzz`, so the shape is fixed in every field's width --
    // which is what stops `guitar.wav`, `mix_track1.wav` and
    // `...track0.wav.bak` from reading as captures. Offering to bin a file the
    // user named themselves is the one mistake this feature may not make.
    static const QRegularExpression re(
        QStringLiteral( "\\A\\d{8}_\\d{6}_\\d{3}_track\\d+\\.wav\\z" ) );
    return re.match( baseName ).hasMatch();
}

QString captureDirOf( const SProject *project )
{
    // The project's OWN directory, which is the second of the recorder's three
    // candidates and the only one that is a property of the project rather
    // than of the run (`SAudioRecorder::start` prefers the test output dir and
    // falls back to Documents). A scan can only speak about the project, so an
    // untitled project -- no directory -- has nothing to scan, and that is
    // reported as an empty list rather than as a guess.
    return project ? project->projectDir() : QString();
}

QList<Orphan> scanUnused( SProject *project )
{
    QList<Orphan> out;
    if( !project ) return out;

    // ---- the MODEL side: extern files NO CLIP PLACES ---------------------
    //
    // "PLACED" MEANS A CLIP WINDOWS IT, and that has to be answered by walking
    // the arrangement. `SObject::refCount()` is NOT the question, although it
    // looks like it: the project's own `linkToFile` bookkeeping holds a
    // reference of its own, so an extern file whose every clip has been undone
    // away still reports a positive count. Measured -- a recording undone left
    // the count at 1 and this scan reported nothing at all.
    //
    // A REGISTERED ASSET IS EXEMPT whatever the walk finds: an asset body is
    // something the user named and can place later, so "nothing places it
    // right now" is not a reason to offer to bin it.
    QSet<const SObject *> placed;
    collectPlacedContent( project, placed );

    QStringList referenced;
    const QHash<QString, SExternFile *> &xf = project->externFiles();
    for( auto it = xf.cbegin(); it != xf.cend(); ++it ) {
        SExternFile *f = it.value();
        if( !f ) continue;
        const QString path = f->getFileName();
        if( !path.isEmpty() ) referenced.append( QFileInfo( path ).fileName() );
        if( placed.contains( f ) ) continue;
        if( !project->assetNameOf( f ).isEmpty() ) continue;
        out.append( { path, QStringLiteral( "no clip places it" ) } );
    }

    // ---- the DISK side: capture files nothing references -----------------
    const QString dir = captureDirOf( project );
    if( dir.isEmpty() ) return out;
    QDir d( dir );
    if( !d.exists() ) return out;
    const QStringList names =
        d.entryList( QStringList() << QStringLiteral( "*.wav" ), QDir::Files,
                     QDir::Name );
    for( const QString &n : names ) {
        if( !isCaptureName( n ) ) continue;
        if( referenced.contains( n ) ) continue;
        out.append( { d.absoluteFilePath( n ),
                      QStringLiteral( "a recording no clip uses" ) } );
    }
    return out;
}

} // namespace srecfiles
