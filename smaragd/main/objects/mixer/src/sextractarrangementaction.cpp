#include "app/objects/mixer/sextractarrangementaction.h"
#include "app/objects/mixer/sdissolvearrangementaction.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"
#include "app/objects/cut/scut.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sarrangements.h"
#include "app/model/sappcontext.h"
#include "app/actions/sactionregistry.h"
#include "tw/core/twfraction.h"
#include "tw/core/twlog.h"
#include <QDomElement>
#include <algorithm>

using namespace strackpath;

namespace {

QString generateArrangementName( SProject *project )
{
    for( int n = 1; ; ++n ) {
        QString candidate = QString( "Arrangement %1" ).arg( n );
        if( !project->hasArrangement( candidate ) ) return candidate;
    }
}

// One half-open span of real material.
struct Span { offset_t begin; offset_t end; };

// Every clip interval under `root`, flattened and merged. Walks lanes
// recursively (a folder's children are lanes too) and takes each non-lane child
// link's [startTime, startTime+duration).
void collectMaterial( SObject *container, QList<Span> &out )
{
    for( SLink *lk : container->childLinks() ) {
        if( !lk ) continue;
        SObject &child = lk->getSObject();
        if( child.isLane() ) {          // a nested lane: recurse
            collectMaterial( &child, out );
            continue;
        }
        if( !lk->hasStartTime() || !child.hasDuration() ) continue;
        const offset_t s = lk->getStartTime();
        const length_t d = child.getDuration();
        if( d <= 0 ) continue;
        out.append( Span{ s, s + (offset_t) d } );
    }
}

QList<Span> mergedMaterial( SObject *root )
{
    QList<Span> raw;
    collectMaterial( root, raw );
    std::sort( raw.begin(), raw.end(),
               []( const Span &a, const Span &b ){ return a.begin < b.begin; } );
    QList<Span> merged;
    for( const Span &s : raw ) {
        if( !merged.isEmpty() && s.begin <= merged.last().end ) {
            if( s.end > merged.last().end ) merged.last().end = s.end;
        } else {
            merged.append( s );
        }
    }
    return merged;
}

// The material spans that fall OUTSIDE [winBegin, winEnd) -- the snippets
// (proposal 09 D20). Each is clipped to the part outside the window, so a clip
// straddling the boundary yields only its overhang.
QList<Span> spansOutside( const QList<Span> &material,
                          offset_t winBegin, offset_t winEnd )
{
    QList<Span> out;
    for( const Span &s : material ) {
        if( s.begin < winBegin ) {
            out.append( Span{ s.begin, std::min( s.end, winBegin ) } );
        }
        if( s.end > winEnd ) {
            out.append( Span{ std::max( s.begin, winEnd ), s.end } );
        }
    }
    return out;
}

}  // namespace

SExtractArrangementAction::SExtractArrangementAction(
        const QString &trackPaths, offset_t rangeStart, offset_t rangeEnd,
        const QString &name, const QString &window, offset_t placeAt )
    : trackPaths_( trackPaths ), rangeStart_( rangeStart ),
      rangeEnd_( rangeEnd ), arrName_( name ), window_( window ),
      placeAt_( placeAt )
{
}

SApplyResult SExtractArrangementAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    SObject *master = project->getRootComponent();
    SStdMixer *masterMixer = dynamic_cast<SStdMixer *>( master );
    if( !masterMixer ) return { false, nullptr };

    const bool wholeExtent = ( window_ == QLatin1String( "extent" ) );
    if( !wholeExtent && rangeEnd_ <= rangeStart_ ) {
        TW_LOGW( "cut", "extract-arrangement: refused, empty range" );
        return { false, nullptr };
    }

    // --- resolve the tracks ------------------------------------------------
    QList<QList<int> > paths;
    for( const QString &p : trackPaths_.split( ';', Qt::SkipEmptyParts ) ) {
        paths.append( stringToPath( p.trimmed() ) );
    }
    if( paths.isEmpty() ) {
        TW_LOGW( "cut", "extract-arrangement: refused, no tracks named" );
        return { false, nullptr };
    }

    QList<STrack *> tracks;
    for( const QList<int> &p : paths ) {
        SObject *obj = resolveByPath( master, p );
        STrack *tk = dynamic_cast<STrack *>( obj );
        if( !tk ) {
            TW_LOGW( "cut", "extract-arrangement: refused, '%s' is not a track",
                     pathToString( p ).toUtf8().constData() );
            return { false, nullptr };
        }
        tracks.append( tk );
    }

    // A folder AND one of its children would be moved twice: the folder move
    // takes the child with it, and the child's own move then detaches it from
    // a parent that is no longer where the path said.
    for( STrack *a : tracks ) {
        for( STrack *b : tracks ) {
            if( a != b && isSelfOrDescendant( a, b ) ) {
                TW_LOGW( "cut", "extract-arrangement: refused, '%s' is inside "
                                "another named track",
                         a->getSName().toUtf8().constData() );
                return { false, nullptr };
            }
        }
    }

    // A track holding a LIVE RECORDING cannot go: SRecordingContent has no
    // loader registration, so an arrangement holding one would not survive a
    // save, and the take is still growing under it.
    for( STrack *tk : tracks ) {
        for( SLink *lk : tk->childLinks() ) {
            if( lk && lk->getSObject().isLiveRecording() ) {
                TW_LOGW( "cut", "extract-arrangement: refused, '%s' holds a "
                                "live recording",
                         tk->getSName().toUtf8().constData() );
                return { false, nullptr };
            }
        }
    }

    const QString arrName = arrName_.isEmpty() ? generateArrangementName( project )
                                               : arrName_;
    if( arrName.contains( QLatin1Char( ':' ) ) ) {
        TW_LOGW( "cut", "extract-arrangement: refused, a name may not contain "
                        "':' (it separates the root from the path): '%s'",
                 arrName.toUtf8().constData() );
        return { false, nullptr };
    }
    if( project->hasArrangement( arrName ) || project->hasAsset( arrName ) ) {
        TW_LOGW( "cut", "extract-arrangement: refused, name '%s' is in use",
                 arrName.toUtf8().constData() );
        return { false, nullptr };
    }

    // --- record the restore plan BEFORE anything moves ---------------------
    // Parent path + index per track, in the order given. This is what makes the
    // inverse exact rather than "append them back somewhere".
    QStringList restore;
    int topMostMasterIndex = masterMixer->getNTracks();
    for( int i = 0; i < tracks.size(); ++i ) {
        QList<int> p = paths.at( i );
        const int idx = p.isEmpty() ? -1 : p.last();
        QList<int> parentPath = p;
        if( !parentPath.isEmpty() ) parentPath.removeLast();
        restore << ( pathToString( parentPath ) + QLatin1Char( '@' )
                     + QString::number( idx ) );
        if( parentPath.isEmpty() && idx >= 0 )
            topMostMasterIndex = std::min( topMostMasterIndex, idx );
    }
    if( topMostMasterIndex > masterMixer->getNTracks() )
        topMostMasterIndex = masterMixer->getNTracks();

    // --- create the arrangement root --------------------------------------
    SStdMixer *arrRoot = new SStdMixer( project );
    arrRoot->setSName( arrName );
    project->registerArrangement( arrName, arrRoot );

    // --- move the tracks, whole (D6) --------------------------------------
    // The detach/attach discipline is SReparentTrackAction's, verbatim,
    // including the pin: detaching drops the old link's reference and
    // removeRef() -> deleteLater() is irreversible, so the count must never
    // touch zero between detach and re-attach.
    for( STrack *tk : tracks ) {
        SObject *parent = nullptr;
        SLink *ownLink = nullptr;
        // Find the link that currently holds it (paths have shifted as earlier
        // tracks were removed, so re-find rather than trust the recorded index).
        for( int i = 0; i < paths.size() && !ownLink; ++i ) { (void) i; }
        // Search the whole master tree for the link naming this track.
        std::function<bool(SObject *)> findLink = [&]( SObject *container ) -> bool {
            for( SLink *lk : container->childLinks() ) {
                if( !lk ) continue;
                if( &lk->getSObject() == static_cast<SObject *>( tk ) ) {
                    parent = container; ownLink = lk; return true;
                }
                SObject &child = lk->getSObject();
                if( child.isLane() && findLink( &child ) ) return true;
            }
            return false;
        };
        findLink( master );
        if( !ownLink || !parent ) continue;   // already moved (nested) -- skip

        tk->addRef();                          // pin across the move
        if( SStdMixer *srcMixer = dynamic_cast<SStdMixer *>( parent ) ) {
            srcMixer->removeTrack( *ownLink );
            QObject::disconnect( tk, nullptr, srcMixer, nullptr );
        } else {
            delete ownLink;
            QObject::disconnect( tk, nullptr, parent, nullptr );
        }
        arrRoot->insertTrack( *tk );
        tk->removeRef();                       // the new link holds it now
    }

    // --- the window --------------------------------------------------------
    const offset_t winBegin = wholeExtent ? 0 : rangeStart_;
    const offset_t winEnd   = wholeExtent ? (offset_t) arrRoot->getDuration()
                                          : rangeEnd_;
    const length_t winLen   = (length_t) std::max<offset_t>( 0, winEnd - winBegin );
    if( winLen <= 0 ) {
        TW_LOGW( "cut", "extract-arrangement: refused, empty window" );
        // The tracks are already moved; dissolve to put them back rather than
        // leave the project half-extracted.
        SDissolveArrangementAction undo( arrName, restore.join( ';' ) );
        undo.apply( project );
        return { false, nullptr };
    }

    // --- the asset ---------------------------------------------------------
    SCut *cut = new SCut( project, *arrRoot );
    cut->setWindow( Fraction( (int64_t) winBegin ), ClipLen( winLen ),
                    WarpedLen( 0 ), Fraction( 1 ) );
    cut->setSName( arrName );
    project->registerAsset( arrName, cut );

    // --- one new master lane, where the topmost extracted track was --------
    STrack *lane = new STrack( project );
    lane->setSName( arrName );
    masterMixer->insertTrack( *lane );
    const int landing = masterMixer->getNTracks() - 1;
    if( topMostMasterIndex < landing )
        masterMixer->reorderTrack( landing, topMostMasterIndex );

    SLink *placement = new SLink( *cut, nullptr );
    placement->setStartTime( placeAt_ < 0 ? winBegin : placeAt_ );
    placement->setParent( lane );

    // --- snippets (D20) ----------------------------------------------------
    // Material outside the window is not lost and is not placed: it becomes a
    // named, UNPLACED asset per contiguous span, for the user to drop in if and
    // where they want it.
    int snippetCount = 0, snippetsDropped = 0;
    offset_t framesOutside = 0;
    if( snippets_ ) {
        const QList<Span> outside =
            spansOutside( mergedMaterial( arrRoot ), winBegin, winEnd );
        for( const Span &s : outside ) {
            if( s.end <= s.begin ) continue;
            framesOutside += ( s.end - s.begin );
            if( snippetCount >= kMaxSnippets ) { ++snippetsDropped; continue; }
            const QString sName = QString( "%1 snippet %2" )
                                      .arg( arrName ).arg( snippetCount + 1 );
            if( project->hasAsset( sName ) ) { ++snippetsDropped; continue; }
            SCut *sc = new SCut( project, *arrRoot );
            sc->setWindow( Fraction( (int64_t) s.begin ),
                           ClipLen( (length_t)( s.end - s.begin ) ),
                           WarpedLen( 0 ), Fraction( 1 ) );
            sc->setSName( sName );
            project->registerAsset( sName, sc );
            ++snippetCount;
        }
    }

    // --- settle both roots -------------------------------------------------
    // BOTH summing roots have to be staled explicitly, and the master one is
    // the load-bearing half. Its cached pages hold the summed mix INCLUDING the
    // tracks that just left, and nothing in removeTrack()'s path bumps the
    // mixer's own epoch -- so a project that had already been rendered or
    // played kept serving those pages and the extracted material was still
    // heard where it used to be. Measured: with a baseline render before the
    // extraction, [0,4s) came back at full level; without one it was silent.
    // That is what extract_arrangement_detached's silence assertions catch.
    arrRoot->applyAudibility();
    arrRoot->notifyTreeChanged();
    arrRoot->bumpRenderChainEpoch();
    masterMixer->applyAudibility();
    masterMixer->notifyTreeChanged();
    masterMixer->bumpRenderChainEpoch();
    SAppContext::get().rewireSpeaker();

    // --- report (D7 / D15): announced, never silent ------------------------
    TW_LOGI( "cut", "extract-arrangement '%s': %d track(s) moved, window "
                    "[%lld,%lld), %d snippet(s) registered for %lld frame(s) of "
                    "material outside it%s",
             arrName.toUtf8().constData(), (int) tracks.size(),
             (long long) winBegin, (long long) winEnd,
             snippetCount, (long long) framesOutside,
             snippetsDropped ? " (SNIPPET CAP REACHED, some spans dropped)" : "" );
    if( !wholeExtent ) {
        // The consequence nobody expects: SProject::getDurationFrames() measures
        // the MASTER root, and SRenderAction defaults its end to it and "narrows
        // a render, it never widens one". So a range window shortens every
        // subsequent whole-project render.
        TW_LOGI( "cut", "extract-arrangement '%s': the project's render length "
                        "now follows this window unless another lane runs longer",
                 arrName.toUtf8().constData() );
    }

    // Remember WHERE these tracks came from, on the project, so that a
    // dissolve the USER invokes later restores the arrangement instead of
    // appending its tracks to the end of the master. Undo carries the plan in
    // its own inverse and does not need this; a user's dissolve, days later
    // and after a save, has no other source for it. The property store is
    // JSON-serialized with the project, so it survives the round trip.
    project->setProp( QStringLiteral( "arrangement/restorePlan/" ) + arrName,
                      restore.join( ';' ) );

    return { true, new SDissolveArrangementAction( arrName, restore.join( ';' ) ) };
}

void SExtractArrangementAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPaths", trackPaths_ );
    elem.setAttribute( "rangeStart", QString::number( (qlonglong) rangeStart_ ) );
    elem.setAttribute( "rangeEnd",   QString::number( (qlonglong) rangeEnd_ ) );
    if( !arrName_.isEmpty() ) elem.setAttribute( "name", arrName_ );
    elem.setAttribute( "window", window_ );
    if( placeAt_ >= 0 ) elem.setAttribute( "placeAt", QString::number( (qlonglong) placeAt_ ) );
    if( !snippets_ ) elem.setAttribute( "snippets", "0" );
}

bool SExtractArrangementAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPaths_ = elem.attribute( "trackPaths" );
    rangeStart_ = (offset_t) elem.attribute( "rangeStart", "0" ).toLongLong();
    rangeEnd_   = (offset_t) elem.attribute( "rangeEnd", "0" ).toLongLong();
    arrName_    = elem.attribute( "name" );
    window_     = elem.attribute( "window", "range" );
    placeAt_    = (offset_t) elem.attribute( "placeAt", "-1" ).toLongLong();
    snippets_   = elem.attribute( "snippets", "1" ) != QLatin1String( "0" );
    return true;
}

static const bool s_reg_extractarrangement = (
    SActionRegistry::instance().registerType(
        QStringLiteral("extract-arrangement"),
        []{ return new SExtractArrangementAction; }
    ), true
);
