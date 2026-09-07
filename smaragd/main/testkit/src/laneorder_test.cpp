// Gate for proposal 48 M0 — the two shared seams, before there is a second
// mount of either.
//
//   AC0.2  `slaneorder::flattenTrackLanes()` produces the arranger's own lane
//          order over a fixture holding folders, a COLLAPSED folder, a HIDDEN
//          lane and the master lane. The arranger's `rebuildRows()` is built
//          from exactly this call, so what is pinned here is what it draws.
//
//   AC0.4  `strackbroadcast::orderByLane()` keeps "a track with no visible
//          lane sorts LAST, stably" (D3a). Before M0 that behaviour came out
//          of `rowIndexOfTrack()` answering -1, i.e. it was an ARTIFACT of
//          reading the view's row list rather than a decision. It is
//          PRESERVED, and this file is the only thing that says so: the one
//          shape that distinguishes the two orders is a multi-selection
//          spanning a collapsed folder, and NOTHING in the qxa suite covers
//          that.
//
// Also pinned, because each is a rule a second mount could quietly re-decide:
// D2 (the mixer ignores FOLD), D6a (the mixer shows the master lane even
// though it is hidden by default, and that exemption does NOT descend to a
// conductor lane), the hidden-takes-its-subtree rule, and the system tail's
// position and order.
//
// No display, no audio device — same shape as fragment_test.

#include "app/model/sappcontext.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/slaneorder.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/mixer/strackbroadcast.h"
#include "app/objects/track/strack.h"

#include "tw/graph/tw303aenv.h"

#include <QApplication>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <memory>

namespace {

int g_failures = 0;

void check( bool ok, const char *what )
{
    if( ok ) { std::printf( "ok   %s\n", what ); return; }
    std::printf( "FAIL %s\n", what );
    ++g_failures;
}

// Same narrow stub fragment_test and project_channels_test use. The
// setBufferSize() call is load-bearing: `tw303aEnvironment::bufferSize` has no
// default member initializer, and SStdMixer's ctor chain reads it immediately
// (fragment_test measured ~1 crash in 25-40 runs without it).
class StubAppContext : public SAppContext {
public:
    StubAppContext() { env_.setBufferSize( 4096 ); }

    SProject *getCurrentProject() const override { return nullptr; }
    tw303aEnvironment *get303aEnvironment() const override { return &env_; }
    void rewireSpeaker() override {}
    bool isSLinkSelected( SLink * ) const override { return false; }
    void setSelectionFromPaths( const QList<QList<int>> & ) override {}
    void addSelectionFromPaths( const QList<QList<int>> & ) override {}
    void removeSelectionFromPaths( const QList<QList<int>> & ) override {}
    void toggleSelectionFromPaths( const QList<QList<int>> & ) override {}
    QList<QList<int>> getCurrentSelectionPaths() const override { return {}; }
    QString testOutputDir() const override { return QString(); }
    bool ensureOutputDirExists() const override { return false; }
    void startRender( const audio::RenderParams & ) override {}
    bool isRenderingActive() const override { return false; }
    void setPlaybackRunning( bool ) override {}
    offset_t getGlobalLocatorPos() const override { return 0; }

private:
    mutable tw303aEnvironment env_;
};

SLink *placeChild( SObject *content, SObject *parent )
{
    SLink *lk = new SLink( *content, nullptr );
    lk->setStartTime( 0 );
    lk->setParent( parent );
    return lk;
}

STrack *addTrack( SProject &p, SObject *parent, const QString &name )
{
    STrack *t = new STrack( &p );
    t->setSName( name );
    placeChild( t, parent );
    return t;
}

/// The lane list as "Name@depth" strings — readable in a failure, and it
/// pins DEPTH as well as order, which is what the arranger's indentation and
/// `laneGroupHeight` are computed from.
QString describe( const QVector<slaneorder::Lane> &lanes )
{
    QStringList out;
    for( const slaneorder::Lane &l : lanes )
        out << QStringLiteral( "%1@%2" )
                   .arg( l.track ? l.track->getSName() : QStringLiteral( "?" ) )
                   .arg( l.depth );
    return out.join( QStringLiteral( " " ) );
}

QString names( const QList<STrack *> &ts )
{
    QStringList out;
    for( STrack *t : ts ) out << ( t ? t->getSName() : QStringLiteral( "?" ) );
    return out.join( QStringLiteral( " " ) );
}

// ---------------------------------------------------------------------------
// The fixture. One shape, exercised by every test below:
//
//   Alpha                     a plain lane
//   Folder                    a folder, EXPANDED
//     FolderKid
//   Collapsed                 a folder, COLLAPSED  <- its child has no row
//     InsideCollapsed
//   Hidden                    laneHidden() == true <- and its child goes too
//     UnderHidden
//   Omega                     a plain lane
//   ---- system tail ----
//   Master                    hidden BY DEFAULT (SStdMixer's ctor)
//     Conductor               an ordinary child link of the master
// ---------------------------------------------------------------------------
struct Fixture {
    std::unique_ptr<SStdMixer> mixer;
    STrack *alpha = nullptr, *folder = nullptr, *folderKid = nullptr;
    STrack *collapsed = nullptr, *insideCollapsed = nullptr;
    STrack *hidden = nullptr, *underHidden = nullptr, *omega = nullptr;
    STrack *master = nullptr, *conductor = nullptr;
};

Fixture makeFixture( SProject &p )
{
    Fixture f;
    f.mixer.reset( new SStdMixer( &p ) );
    SStdMixer *m = f.mixer.get();

    f.alpha           = addTrack( p, m, QStringLiteral( "Alpha" ) );
    f.folder          = addTrack( p, m, QStringLiteral( "Folder" ) );
    f.folderKid       = addTrack( p, f.folder, QStringLiteral( "FolderKid" ) );
    f.collapsed       = addTrack( p, m, QStringLiteral( "Collapsed" ) );
    f.insideCollapsed = addTrack( p, f.collapsed, QStringLiteral( "InsideCollapsed" ) );
    f.hidden          = addTrack( p, m, QStringLiteral( "Hidden" ) );
    f.underHidden     = addTrack( p, f.hidden, QStringLiteral( "UnderHidden" ) );
    f.omega           = addTrack( p, m, QStringLiteral( "Omega" ) );

    f.collapsed->setCollapsed( true );
    f.hidden->setLaneHidden( true );

    f.master = m->masterLane();
    // The conductor lane the mixer's ctor mints as an ordinary child of the
    // master (proposal 45 M6). Named here so a failure reads.
    for( SLink *lk : f.master->childLinks() )
        if( STrack *t = dynamic_cast<STrack *>( &lk->getSObject() ) )
            f.conductor = t;
    return f;
}

// --- AC0.2: the ARRANGER's order -------------------------------------------

void testArrangerOrder( SProject &p )
{
    Fixture f = makeFixture( p );

    // Exactly what SStdMixerView::rebuildRows() passes.
    slaneorder::Options opt;
    opt.fold             = slaneorder::Fold::Honour;
    opt.hidden           = slaneorder::Hidden::Honour;
    opt.system           = slaneorder::SystemLanes::MasterSubtree;
    opt.alwaysShowMaster = false;

    const QVector<slaneorder::Lane> lanes =
        slaneorder::flattenTrackLanes( f.mixer.get(), opt );
    const QString got = describe( lanes );

    // A COLLAPSED folder keeps its own lane and drops its children; a HIDDEN
    // lane takes its subtree with it; the master is hidden by default, so on a
    // project nobody has touched there is NO system tail at all. That last
    // clause is D6a's whole reason for existing.
    check( got == QStringLiteral( "Alpha@0 Folder@0 FolderKid@1 Collapsed@0 Omega@0" ),
           qPrintable( QStringLiteral(
               "AC0.2: the arranger's lane order, depth included "
               "[got: %1]" ).arg( got ) ) );

    check( slaneorder::indexOfTrack( lanes, f.insideCollapsed ) < 0,
           "AC0.2: a COLLAPSED folder's child has no lane" );
    check( slaneorder::indexOfTrack( lanes, f.hidden ) < 0
           && slaneorder::indexOfTrack( lanes, f.underHidden ) < 0,
           "AC0.2: a HIDDEN lane takes its subtree with it" );
    check( slaneorder::indexOfTrack( lanes, f.master ) < 0,
           "D6a: the master lane is hidden BY DEFAULT, so the arranger shows "
           "no master row on an untouched project -- which is exactly why the "
           "mixer pane needs an exemption rather than inheriting this" );
}

// --- AC0.2: the system tail, once the master is shown -----------------------

void testSystemTail( SProject &p )
{
    Fixture f = makeFixture( p );
    f.master->setLaneHidden( false );

    // HIDDEN IS PER LANE, AND THE CONDUCTOR HAS ITS OWN. `laneHiddenByDefault()`
    // is true for EVERY system role, so un-hiding the master does not drag its
    // conductor lane into view -- which is what the arranger already does
    // (a hidden child is skipped like any other) and is worth pinning,
    // because the natural expectation is the opposite. Asserted before the
    // conductor is un-hidden, so the check below cannot pass vacuously.
    const QString masterOnly =
        describe( slaneorder::flattenTrackLanes( f.mixer.get(), slaneorder::Options() ) );
    check( masterOnly == QStringLiteral( "Alpha@0 Folder@0 FolderKid@1 Collapsed@0 "
                                         "Omega@0 Master@0" ),
           qPrintable( QStringLiteral(
               "AC0.2: un-hiding the MASTER does not un-hide its CONDUCTOR "
               "lane -- hidden is per lane [got: %1]" ).arg( masterOnly ) ) );

    f.conductor->setLaneHidden( false );
    const QVector<slaneorder::Lane> lanes =
        slaneorder::flattenTrackLanes( f.mixer.get(), slaneorder::Options() );
    const QString got = describe( lanes );

    check( got == QStringLiteral( "Alpha@0 Folder@0 FolderKid@1 Collapsed@0 Omega@0 "
                                  "Master@0 Conductor@1" ),
           qPrintable( QStringLiteral(
               "AC0.2: the system tail comes LAST, below every user lane "
               "(proposal 45 AC4.1), with the conductor lane as an ordinary "
               "child at depth 1 [got: %1]" ).arg( got ) ) );

    const int mi = slaneorder::indexOfTrack( lanes, f.master );
    check( mi >= 0 && lanes[mi].link == nullptr,
           "The master lane carries a NULL link: it is not a child of the "
           "mixer, it is the mixer's own output stage (45 D2)" );
    check( mi >= 0 && lanes[mi].role == SSystemRole::Master,
           "...and its systemRole() rides along, so a consumer need not "
           "re-derive it" );

    const int ci = slaneorder::indexOfTrack( lanes, f.conductor );
    check( ci >= 0 && lanes[ci].link != nullptr && lanes[ci].parent == f.master,
           "A conductor lane is an ORDINARY lane: a real SLink under a real "
           "container, so a gesture on it derives its commit address by the "
           "ordinary route (45 D9 answers `$master,0`) rather than `{}`" );

    // Collapsing the master hides the conductor lane, exactly as a folder
    // hides its children -- so the fold triangle means one thing everywhere.
    // Only meaningful now that the conductor is visible in the first place.
    f.master->setCollapsed( true );
    const QString folded =
        describe( slaneorder::flattenTrackLanes( f.mixer.get(), slaneorder::Options() ) );
    check( folded == QStringLiteral( "Alpha@0 Folder@0 FolderKid@1 Collapsed@0 "
                                     "Omega@0 Master@0" ),
           qPrintable( QStringLiteral(
               "A COLLAPSED master lane hides its (now visible) conductor "
               "lane [got: %1]" ).arg( folded ) ) );
}

// --- D2 / D6a: the MIXER PANE's order ---------------------------------------

void testMixerPaneOrder( SProject &p )
{
    Fixture f = makeFixture( p );

    // What the pane will pass: fold ignored (D2 -- a mixer has no rows, so
    // "show this lane's children as rows" is not a question it can answer),
    // hidden honoured for user lanes, master shown regardless (D6a).
    slaneorder::Options opt;
    opt.fold             = slaneorder::Fold::Ignore;
    opt.hidden           = slaneorder::Hidden::Honour;
    opt.system           = slaneorder::SystemLanes::MasterSubtree;
    opt.alwaysShowMaster = true;

    const QVector<slaneorder::Lane> lanes =
        slaneorder::flattenTrackLanes( f.mixer.get(), opt );
    const QString got = describe( lanes );

    check( got == QStringLiteral( "Alpha@0 Folder@0 FolderKid@1 Collapsed@0 "
                                  "InsideCollapsed@1 Omega@0 Master@0" ),
           qPrintable( QStringLiteral(
               "D2: a COLLAPSED folder's children keep their strips, and D6a: "
               "the master strip is present on a project that has never "
               "called set-lane-hidden [got: %1]" ).arg( got ) ) );

    check( slaneorder::indexOfTrack( lanes, f.hidden ) < 0,
           "D2: HIDDEN is still honoured -- it answers \"show this track at "
           "all\", which both mounts must agree on" );

    // D6a EXEMPTS THE MASTER LANE ITSELF, NEVER ITS CHILDREN. The conductor
    // lane is hidden by default too, and it stays hidden: a mixer without its
    // summing point is not a mixer, but that argument does not reach a
    // conductor lane, which carries no audio at all.
    check( slaneorder::indexOfTrack( lanes, f.conductor ) < 0,
           "D6a: the exemption does NOT descend -- a hidden conductor lane "
           "stays hidden in both mounts" );
}

// --- AC0.4 / D3a: "no visible lane sorts LAST" ------------------------------

void testOrderByLanePreservesNoRowLast( SProject &p )
{
    Fixture f = makeFixture( p );

    // THE ONE SHAPE THAT DISTINGUISHES THE TWO ORDERS, and the reason this
    // assertion is here rather than in the qxa suite: `InsideCollapsed` is a
    // selected track with NO visible lane. Reachable in the app by collapsing
    // a folder AFTER selecting its children.
    //
    // Handed in deliberately-wrong order, so a pass cannot come from the
    // input already being sorted.
    const QList<STrack *> in{ f.omega, f.insideCollapsed, f.alpha, f.folderKid };
    const QString got = names( strackbroadcast::orderByLane( f.mixer.get(), in ) );

    check( got == QStringLiteral( "Alpha FolderKid Omega InsideCollapsed" ),
           qPrintable( QStringLiteral(
               "AC0.4/D3a: lane order, with the no-visible-lane track LAST -- "
               "PRESERVED from the pre-M0 rowIndexOfTrack() behaviour, which "
               "produced it by answering -1 [got: %1]" ).arg( got ) ) );

    // Stability, asserted separately: two tracks with no lane keep their
    // INPUT order relative to each other. `std::stable_sort` gives this, and
    // an implementation that reached for `std::sort` would pass the line
    // above and fail here.
    f.folder->setCollapsed( true );        // FolderKid loses its lane too
    const QList<STrack *> in2{ f.omega, f.insideCollapsed, f.folderKid, f.alpha };
    const QString got2 = names( strackbroadcast::orderByLane( f.mixer.get(), in2 ) );
    check( got2 == QStringLiteral( "Alpha Omega InsideCollapsed FolderKid" ),
           qPrintable( QStringLiteral(
               "AC0.4: two lane-less tracks keep their INPUT order -- stably "
               "[got: %1]" ).arg( got2 ) ) );
}

// --- D3: the broadcast rule -------------------------------------------------

void testTargetsFor( SProject &p )
{
    Fixture f = makeFixture( p );
    SStdMixer *m = f.mixer.get();

    check( names( strackbroadcast::targetsFor( m, f.alpha ) )
               == QStringLiteral( "Alpha" ),
           "D3: with nothing selected, a gesture acts on the lane clicked" );

    m->setSelectedTracks( QList<STrack *>{ f.omega, f.alpha } );
    check( m->nSelectedTracks() == 2, "fixture: two tracks selected" );

    check( names( strackbroadcast::targetsFor( m, f.alpha ) )
               == QStringLiteral( "Alpha Omega" ),
           "D3: a gesture aimed INTO a multi-selection broadcasts over it, in "
           "LANE order rather than selection order" );

    check( names( strackbroadcast::targetsFor( m, f.folder ) )
               == QStringLiteral( "Folder" ),
           "D3: a gesture aimed OUTSIDE the selection acts on that lane "
           "alone -- so an operation can never reach a track the user is not "
           "pointing at" );

    // A single-track selection is not a broadcast: the clicked lane wins,
    // which is what stops a stale one-track selection acting at a distance.
    m->setSelectedTrack( f.omega );
    check( names( strackbroadcast::targetsFor( m, f.alpha ) )
               == QStringLiteral( "Alpha" ),
           "D3: a ONE-track selection does not broadcast" );
}

void testPruneNestedTargets( SProject &p )
{
    Fixture f = makeFixture( p );

    const QList<STrack *> in{ f.folder, f.folderKid, f.alpha };
    check( names( strackbroadcast::pruneNestedTargets( in ) )
               == QStringLiteral( "Folder Alpha" ),
           "D3: a structural verb acts on each subtree ONCE -- a child covered "
           "by a selected ancestor is dropped, and the survivors keep their "
           "input order" );

    const QList<STrack *> flat{ f.alpha, f.omega };
    check( names( strackbroadcast::pruneNestedTargets( flat ) )
               == QStringLiteral( "Alpha Omega" ),
           "...and an unrelated pair is untouched" );
}

}  // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );
    StubAppContext ctx;
    SAppContext::setInstance( &ctx );

    {
        std::unique_ptr<SProject> p( new SProject );
        p->setChannels( 1 );

        testArrangerOrder( *p );
        testSystemTail( *p );
        testMixerPaneOrder( *p );
        testOrderByLanePreservesNoRowLast( *p );
        testTargetsFor( *p );
        testPruneNestedTargets( *p );
    }

    if( g_failures ) {
        std::printf( "\n%d check(s) FAILED\n", g_failures );
        return 1;
    }
    std::printf( "\nall checks passed\n" );
    return 0;
}
