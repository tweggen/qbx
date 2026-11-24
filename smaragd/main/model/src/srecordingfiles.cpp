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
    // check_root_resolution: master -- enumerates EVERY root -- the master here, then project->arrangements() below
    if( SObject *master = project->masterRoot() )
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

    // ---- WHAT IS IN USE: extern files a clip PLACES, and asset bodies ----
    //
    // "PLACED" MEANS A CLIP WINDOWS IT, and that has to be answered by walking
    // the arrangement. `SObject::refCount()` is NOT the question, although it
    // looks like it: the project's own `linkToFile` bookkeeping holds a
    // reference of its own, so an extern file whose every clip has been undone
    // away still reports a positive count. Measured -- a recording undone left
    // the count at 1 and this scan reported nothing at all.
    //
    // A REGISTERED ASSET IS IN USE whatever the walk finds: an asset body is
    // something the user named and can place later, so "nothing places it
    // right now" is not a reason to offer to bin it.
    //
    // This is the ONLY thing the objects are asked (QBX-315). A file in use is
    // held by a live clip or a pinned asset, so asking about it is asking about
    // objects that are alive BECAUSE they are in use. What is NOT in use is
    // the opposite: its objects are garbage, and whether a DeferredDelete has
    // reached them yet is timing. So the list itself is read from the disk.
    QSet<const SObject *> placed;
    collectPlacedContent( project, placed );

    QSet<QString> usedNames;
    QList<SExternFile *> unused;
    const QHash<QString, SExternFile *> &xf = project->externFiles();
    for( auto it = xf.cbegin(); it != xf.cend(); ++it ) {
        SExternFile *f = it.value();
        if( !f ) continue;
        if( placed.contains( f ) || !project->assetNameOf( f ).isEmpty() ) {
            const QString path = f->getFileName();
            if( !path.isEmpty() ) usedNames.insert( QFileInfo( path ).fileName() );
        } else {
            unused.append( f );
        }
    }

    // ---- the DISK side: capture files nothing uses -----------------------
    //
    // TWO PLACES TO LOOK, both of them directories and paths, never objects:
    // the project's own directory (a saved project, and one reopened), and the
    // files this document's recorder wrote (SProject::captureFiles), which is
    // the only way to find the captures of an UNTITLED project -- it records
    // into Documents or a test run's output dir, and sweeping either would
    // offer other projects' recordings. Each candidate must exist, must pass
    // the strict capture-name test, and must not share a NAME with a file in
    // use (by name and not by path, deliberately: a capture name carries a
    // millisecond stamp, so a match is the same take, and when in doubt this
    // list leaves a file off rather than offering it for the bin).
    QStringList candidates;
    const QString dir = captureDirOf( project );
    if( !dir.isEmpty() ) {
        QDir d( dir );
        if( d.exists() ) {
            const QStringList names =
                d.entryList( QStringList() << QStringLiteral( "*.wav" ),
                             QDir::Files, QDir::Name );
            for( const QString &n : names ) candidates << d.absoluteFilePath( n );
        }
    }
    for( const QString &p : project->captureFiles() )
        candidates << QFileInfo( p ).absoluteFilePath();

    QSet<QString> listed;
    QList<Orphan> disk;
    for( const QString &abs : candidates ) {
        const QString clean = QDir::cleanPath( abs );
        if( listed.contains( clean ) ) continue;
        const QFileInfo fi( clean );
        if( !isCaptureName( fi.fileName() ) ) continue;
        if( !fi.isFile() ) continue;
        if( usedNames.contains( fi.fileName() ) ) continue;
        listed.insert( clean );
        disk.append( { clean, QStringLiteral( "a recording no clip uses" ) } );
    }

    // ---- the MODEL side: what the disk cannot see ------------------------
    //
    // An extern file the project still holds with nothing placing it, which
    // the disk half did not already list: a user's own sample whose clips
    // were all deleted, or a recording whose file has been moved away. Never
    // a second entry for a capture the disk half found, so the count and the
    // wording of a recording's entry do not depend on its objects.
    for( SExternFile *f : unused ) {
        const QString path = f->getFileName();
        if( listed.contains( QDir::cleanPath( QFileInfo( path ).absoluteFilePath() ) ) )
            continue;
        out.append( { path, QStringLiteral( "no clip places it" ) } );
    }
    out.append( disk );
    return out;
}

} // namespace srecfiles
