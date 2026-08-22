// Gate for proposal 41 M1 — SLaneFragment, a path container with no track
// identity. Four things are pinned, one per AC:
//
//   AC1.1 isPathContainer()==true / isLane()==false (the proposal 41 D3
//         split this milestone is the FIRST to exercise with a real second
//         answer), and a fragment sums its audio children at UNITY through a
//         twTrackMix — a closed-form overlap sum against a synthetic fixture
//         source, so a wrong mixdown shows up as a wrong VALUE, not silence.
//   AC1.2 the absence of a fader/inserts/instrument/solo/arm/automation is
//         asserted STRUCTURALLY: the project-wide solo walk and the
//         edit-group walk never find a fragment's own flags (they are
//         isLane()-gated, and a fragment answers that false — proving the
//         gate actually does something, not merely that it exists), and
//         setVolume()/an automation lane on the fragment change NOTHING in
//         the rendered output (nothing wires either into the mix — a fader
//         bolted on later would flip this).
//   AC1.3 a full save/load round trip through the REAL SProjectLoader,
//         including a child <SLink> whose objectId the file does not
//         contain — the fragment (SElementKind::Container) survives with
//         its OTHER child intact, matching how a track or a mixer already
//         tolerates one unresolvable clip.
//   AC1.4 an SCut constructed over a fragment behaves exactly like one
//         constructed over any other container: its content is the
//         fragment, and its default window duration is the fragment's own
//         (children-extent) duration — the same rule screateassetaction.cpp
//         relies on when it windows an existing folder track.
//
// No display, no audio device — same shape as project_channels_test.

#include "app/model/sappcontext.h"
#include "app/model/sautomationlane.h"
#include "app/model/seditgroups.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/ssolorules.h"
#include "app/objects/cut/scut.h"
#include "app/objects/fragment/slanefragment.h"
#include "app/objects/mixer/spackselectionaction.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidisequence.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/persistence/sprojectloader.h"

#include "tw/events/twautomationcurve.h"
#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
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

bool nearlyEqual( float a, float b )
{
    return std::fabs( a - b ) < 1e-4f;
}

// The narrow app context the model layer asserts on (SAppContext::get()) —
// same stub project_channels_test uses. SLaneFragment's constructor reaches
// SAppContext::get().get303aEnvironment() exactly as STrack's does.
class StubAppContext : public SAppContext {
public:
    StubAppContext()
    {
        // tw303aEnvironment::bufferSize has NO default member initializer and
        // its constructor never sets one (tw303aenv.cc only sets sampleRate)
        // — reading it before ANY setBufferSize() call is UNINITIALIZED
        // memory. project_channels_test's identical stub never trips this
        // because it only ever builds an STrack; this file is the first
        // off-the-app-proper fixture to construct a REAL SStdMixer, whose
        // ctor chain (twMixer::twMixer -> setBufferSize(env.getBufferSize()))
        // reads it immediately. Measured: ~1 crash in 25-40 runs (calloc
        // asked for a garbage-huge size and returned null, thrown as
        // excStandard, uncaught) — a genuine, pre-existing latent bug in
        // tw303aEnvironment, found by this milestone but NOT this
        // milestone's to fix (out of scope for SLaneFragment; flagged in the
        // M1 report). 4096 matches sapplication.cpp / playback_test.cc's own
        // stub.
        env_.setBufferSize( 4096 );
    }

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

// --- a synthetic audio-bearing "clip" fixture, mirroring mix_test.cc's
// RampComponent / preview_container_test.cpp's ContainerObject: a constant,
// never-silent amplitude so a wrong mixdown reads as a wrong VALUE.

class ConstComponent : public twComponent {
public:
    ConstComponent( tw303aEnvironment &e, float amplitude )
        : twComponent( e ), amplitude_( amplitude ) {}

    bool isSeekable() const override { return true; }
    int seekTo( offset_t ) override { return 0; }
    void reset() override {}

    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override
    {
        for( length_t i = 0; i < n; ++i ) out[i] = amplitude_;
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "const"; }

private:
    float amplitude_;
};

// A project-less content object (SObject(nullptr), exactly as
// preview_container_test's ContainerObject is) — it needs no tree position
// of its own to be wired as a fragment's child via a plain SLink.
class FixtureClip : public SObject {
public:
    FixtureClip( tw303aEnvironment &env, float amplitude, length_t duration )
        : SObject( nullptr ),
          comp_( std::make_shared<ConstComponent>( env, amplitude ) ),
          duration_( duration )
    {
        comp_->init();
    }

    std::shared_ptr<twComponent> getRootComponent() override { return comp_; }
    bool hasDuration() const override { return true; }
    length_t getDuration() const override { return duration_; }

    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override { return nullptr; }

private:
    std::shared_ptr<ConstComponent> comp_;
    length_t duration_;
};

SLink *placeChild( SObject *content, offset_t start, SObject *parent )
{
    SLink *lk = new SLink( *content, nullptr );
    lk->setStartTime( start );
    lk->setParent( parent );
    return lk;
}

// --- AC1.1 -------------------------------------------------------------

void testStructuralPredicates( SProject &project )
{
    std::unique_ptr<SLaneFragment> fragment( new SLaneFragment( &project ) );
    check( fragment->isPathContainer(),
           "AC1.1: a fragment answers isPathContainer() true" );
    check( !fragment->isLane(),
           "AC1.1/D3: a fragment answers isLane() false (the split M0 built "
           "for exactly this moment)" );
}

void testUnitySumAndNoOwnProcessing( SProject &project )
{
    tw303aEnvironment *env = SAppContext::get().get303aEnvironment();

    std::unique_ptr<SLaneFragment> fragment( new SLaneFragment( &project ) );
    check( fragment->getRootComponent() != nullptr,
           "AC1.1: a fragment hands out a non-null root component" );

    // A [0,2000) amplitude 0.2, B [1000,3000) amplitude 0.3 — overlap
    // [1000,2000) must read the raw SUM (0.5), never an attenuated one: a
    // fader anywhere in the chain would fail this closed form.
    FixtureClip *a = new FixtureClip( *env, 0.2f, 2000 );
    placeChild( a, 0, fragment.get() );
    FixtureClip *b = new FixtureClip( *env, 0.3f, 2000 );
    placeChild( b, 1000, fragment.get() );

    const length_t PAGE = 4000;
    auto page = fragment->getRootComponent()->freezePage(
        0, nullptr, 0, PAGE, env->getSRate(), nullptr );
    check( page && page->validFrames == PAGE, "the fragment's root freezes a page" );
    if( !page ) return;

    const float *s = page->channelPtr( 0 );
    check( nearlyEqual( s[0], 0.2f ) && nearlyEqual( s[999], 0.2f ),
           "AC1.1: A alone reads its own amplitude" );
    check( nearlyEqual( s[1000], 0.5f ) && nearlyEqual( s[1999], 0.5f ),
           "AC1.1: the overlap is the UNITY SUM of A and B (0.2+0.3)" );
    check( nearlyEqual( s[2000], 0.3f ) && nearlyEqual( s[2999], 0.3f ),
           "AC1.1: B alone reads its own amplitude after A ends" );
    check( s[3000] == 0.0f && s[3999] == 0.0f,
           "AC1.1: silence once both clips have ended (no bleed)" );

    // AC1.2: no fader. setVolume() is the generic SObject property STrack's
    // OWN fader wires to (onTrackVolumeChanged) — SLaneFragment connects
    // nothing to it, so a -20 dB setting must leave the mix untouched. A
    // future "bolt a fader on" that reintroduced that wiring would turn this
    // into a failing 0.2/0.5/0.3 vs. ~0.02/0.05/0.03 mismatch.
    fragment->setVolume( -20.0 );
    auto page2 = fragment->getRootComponent()->freezePage(
        0, nullptr, 0, PAGE, env->getSRate(), nullptr );
    const float *s2 = page2->channelPtr( 0 );
    check( nearlyEqual( s2[0], 0.2f ) && nearlyEqual( s2[1500], 0.5f )
              && nearlyEqual( s2[2500], 0.3f ),
           "AC1.2: setVolume() on the fragment changes NOTHING in the mix "
           "(no fader stage exists to apply it)" );
    fragment->setVolume( 0.0 );

    // AC1.2: no automation of its own. A self:Volume lane is the generic
    // SObject automation API (proposal 37 P5) every object can technically
    // hold — nothing here CONSUMES it (a fragment has no gain stage to feed
    // it into), so adding one must be equally inert.
    SAutomationLane *lane = fragment->ensureAutomationLane(
        QStringLiteral( "self:Volume" ) );
    check( lane != nullptr, "the generic automation API accepts the call" );
    if( lane ) {
        lane->setPoints( { { 0, -40.0, twCurveShape::Step } } );
    }
    auto page3 = fragment->getRootComponent()->freezePage(
        0, nullptr, 0, PAGE, env->getSRate(), nullptr );
    const float *s3 = page3->channelPtr( 0 );
    check( nearlyEqual( s3[0], 0.2f ) && nearlyEqual( s3[1500], 0.5f )
              && nearlyEqual( s3[2500], 0.3f ),
           "AC1.2: an automation lane on the fragment changes NOTHING in "
           "the mix (no consumer reads it)" );
}

// AC1.2 continued: the SOLO / EDIT-GROUP walks are isLane()-gated
// (ssolorules.h, seditgroups.h), and a fragment answers isLane() false — so
// even a fragment whose own solo/edit-group flag is set (the generic base
// SObject setters accept the call; nothing stops it at compile time) has
// ZERO effect on the project-wide rule. Placed as a DIRECT child of a track
// (bypassing the SCut a real placement always interposes, proposal 41 D2) so
// the walk actually reaches the fragment and this exercises isLane() itself,
// not merely the fact that an SCut sits in the way in production.
void testLaneStateNeverLeaksFromFragment( SProject &project )
{
    std::unique_ptr<SStdMixer> root( new SStdMixer( &project ) );
    STrack *track = new STrack( &project );
    placeChild( track, 0, root.get() );

    SLaneFragment *fragment = new SLaneFragment( &project );
    placeChild( fragment, 0, track );

    fragment->setSolo( true );
    check( !ssolo::anySoloInTree( root.get() ),
           "AC1.2: a fragment's own solo flag never lights up the "
           "project-wide solo walk (isLane()==false stops it here)" );
    fragment->setSolo( false );
    check( !ssolo::anySoloInTree( root.get() ),
           "...and clearing it leaves the (already false) answer unchanged" );

    fragment->setEditGroup( 7 );
    QList<SObject *> members;
    seditgroups::membersOf( root.get(), 7, members );
    check( members.isEmpty(),
           "AC1.2: a fragment's edit-group id is never collected as lane "
           "membership" );

    check( splacements::laneAt( track, QList<int>{ 0 } ) == nullptr,
           "AC1.2: a fragment never resolves as a LANE through the "
           "placement service (splacements::laneAt)" );
}

// --- AC1.4 ---------------------------------------------------------------

void testCutWindowsFragmentLikeTrack( SProject &project )
{
    tw303aEnvironment *env = SAppContext::get().get303aEnvironment();

    // Raw `new`, NOT a unique_ptr: `cut` below holds a REFERENCE to this
    // fragment (its content_ link), and a unique_ptr's synchronous delete()
    // at this function's scope exit would free it out from under that
    // reference — a dangling SLink that only bites at process teardown,
    // exactly the class of bug the house rule about "passes, then crashes on
    // exit" warns about. Every object in this function is left for `project`
    // (its real QObject parent, alive for the whole test) to reap, matching
    // the rest of this file's fixtures.
    SLaneFragment *fragment = new SLaneFragment( &project );
    FixtureClip *a = new FixtureClip( *env, 0.1f, 2000 );
    placeChild( a, 0, fragment );
    FixtureClip *b = new FixtureClip( *env, 0.1f, 2000 );
    placeChild( b, 3000, fragment );
    check( fragment->getDuration() == 5000,
           "the fragment's own duration is the extent of its children (last end)" );

    SCut *cut = new SCut( &project, *fragment );
    check( &cut->getContent() == fragment,
           "AC1.4: the SCut's content IS the fragment" );
    check( cut->getDurationBlocking() == 5000,
           "AC1.4: SCut defaults its window to the CONTAINER's own duration, "
           "exactly as it does for an existing track/mixer (screateassetaction)" );
}

// --- Proposal 41 M2b: containerAt() split + asset-name addressing --------
//
// splacements::laneAt() stays exactly as strict as it always was (AC2b.1);
// the new containerAt() is what widens the CLIP-parent resolution, and
// rootNamed() is extended so an asset name resolves to its FRAGMENT content
// (never a plain container-asset's track/mixer, which already has a name of
// its own). All at the model level, no rendering — the audible-through-every-
// -placement claim (AC2b.3) is the qxa case
// fragment_nested_edit_reaches_all_placements.qxa; this is the resolver
// contract underneath it.
void testAssetNameAddressing( SProject &project )
{
    tw303aEnvironment *env = SAppContext::get().get303aEnvironment();

    // A fragment asset "Riff", two children — the same shape pack-clips
    // builds (spackclipsaction.cpp), assembled by hand here so this stays a
    // pure model-level test with no action registry involved.
    SLaneFragment *fragment = new SLaneFragment( &project );
    FixtureClip *a = new FixtureClip( *env, 0.1f, 2000 );
    SLink *linkA = placeChild( a, 0, fragment );
    FixtureClip *b = new FixtureClip( *env, 0.1f, 2000 );
    placeChild( b, 3000, fragment );

    SCut *riffCut = new SCut( &project, *fragment );
    riffCut->setSName( "Riff" );
    project.registerAsset( "Riff", riffCut );

    // --- AC2b.1: laneAt() is UNCHANGED -------------------------------------
    check( splacements::laneAt( fragment, QList<int>{} ) == nullptr,
           "AC2b.1: laneAt() still refuses a fragment directly" );
    check( splacements::containerAt( fragment, QList<int>{} ) == fragment,
           "...but containerAt() accepts it (isPathContainer(), not isLane())" );

    // --- AC2b.2: "Riff:0"/"Riff:1" address the fragment's own children -----
    SObject *riffRoot = splacements::rootNamed( &project, "Riff" );
    check( riffRoot == fragment,
           "AC2b.2: rootNamed() resolves an asset name to its windowed "
           "FRAGMENT content, not the registered SCut itself" );
    SLink *lk0 = splacements::placementAt( riffRoot, QList<int>{ 0 } );
    check( lk0 == linkA,
           "AC2b.2: \"Riff:0\" (root=Riff, path={0}) addresses the fragment's "
           "FIRST packed clip" );

    // --- Arrangements WIN a name collision ---------------------------------
    // Raw `new`, not a unique_ptr (testCutWindowsFragmentLikeTrack's rule,
    // above): `trackCut` below holds a REFERENCE (its content_ link) to
    // `lane`, and a unique_ptr's synchronous delete at scope exit would free
    // it out from under that reference. Both are left for `project` (their
    // real QObject parent) to reap, as everywhere else in this file.
    SStdMixer *arrangementRoot = new SStdMixer( &project );
    project.registerArrangement( "Riff", arrangementRoot );
    check( splacements::rootNamed( &project, "Riff" ) == arrangementRoot,
           "AC2b.2: an ARRANGEMENT named \"Riff\" wins the collision over the "
           "asset of the same name — every path that already resolved keeps "
           "its meaning" );
    project.unregisterArrangement( "Riff" );
    check( splacements::rootNamed( &project, "Riff" ) == fragment,
           "...and reverts to the asset once the arrangement is gone (proves "
           "the fallback, rather than the arrangement dict, is what matched)" );

    // --- An asset over a plain LANE gains no second name -------------------
    STrack *lane = new STrack( &project );
    FixtureClip *c = new FixtureClip( *env, 0.1f, 500 );
    placeChild( c, 0, lane );
    SCut *trackCut = new SCut( &project, *lane );
    trackCut->setSName( "TrackAsset" );
    project.registerAsset( "TrackAsset", trackCut );
    check( splacements::rootNamed( &project, "TrackAsset" ) == nullptr,
           "AC2b.2: an asset windowing a plain lane (STrack, isLane()==true) "
           "gains NO second name — that lane is already addressable by "
           "itself, and the fragment-only fallback must not fire for it" );
}

// --- AC1.3 -----------------------------------------------------------------

void testSerializeRoundTrip()
{
    QTemporaryDir dir;
    check( dir.isValid(), "a temp dir is available for the round-trip file" );
    if( !dir.isValid() ) return;
    const QString path = dir.filePath( QStringLiteral( "fragment_roundtrip.qxp" ) );

    // Hand-authored, not derived from a live save: full control over the
    // one thing under test — a <SLink> inside the fragment naming an
    // objectId the document never defines. "root" is an otherwise-empty
    // mixer (SProjectLoader::createObjects refuses a document with no
    // resolvable rootId, so SOMETHING must satisfy it); the fragment itself
    // is not reachable from it, deliberately — the loader's flat
    // instantiation loop resolves every element with resolvable link
    // children regardless of whether it hangs off the root (see
    // sprojectloader.cpp; every existing loader function for a document-
    // level object, e.g. SPluginChain, is written the same way).
    const char *xml =
        "<SProject fileName='' rootId='root' sampleRate='48000' channels='1'>\n"
        "  <SStdMixer id='root'>\n"
        "  </SStdMixer>\n"
        "  <SMidiSequence id='seq1'>\n"
        "  </SMidiSequence>\n"
        "  <SMidiCut id='midicut1'>\n"
        "    <SLink objectId='seq1'/>\n"
        "  </SMidiCut>\n"
        "  <SLaneFragment id='frag1'>\n"
        "    <SLink objectId='midicut1' startTime='500'/>\n"
        "    <SLink objectId='ghost999' startTime='2000'/>\n"
        "  </SLaneFragment>\n"
        "  <SCut id='cut1'>\n"
        "    <SLink objectId='frag1'/>\n"
        "  </SCut>\n"
        "</SProject>\n";

    {
        QFile f( path );
        check( f.open( QIODevice::WriteOnly | QIODevice::Text ),
               "the fixture document opens for writing" );
        f.write( xml );
        f.close();
    }

    SProject loaded;
    SProjectLoader loader( loaded, path );
    check( loader.wasLoaded(), "the fixture document parses as XML" );
    const int rc = loader.createObjects( loaded );
    check( rc == 0, "createObjects() succeeds despite the dangling SLink" );

    // Assertions run while `loader` is still alive: its object dictionary
    // holds the handle SLinks that keep every loaded object's refcount
    // positive (see ~SProjectLoader's own comment) until it is destroyed.
    SLink *fragLink = loader.getObjectDictionary().value( QStringLiteral( "frag1" ) );
    check( fragLink != nullptr, "AC1.3: the fragment element round-trips at all" );
    if( !fragLink ) return;

    SLaneFragment *fragment = dynamic_cast<SLaneFragment *>( &fragLink->getSObject() );
    check( fragment != nullptr, "...as an SLaneFragment specifically" );
    if( !fragment ) return;

    check( fragment->isPathContainer() && !fragment->isLane(),
           "AC1.3: the RELOADED fragment still answers isPathContainer()/"
           "isLane() correctly (not merely the freshly-constructed one)" );

    // The dangling link ("ghost999") must be gone; the KNOWN child
    // ("midicut1") must survive, at its authored position — verbatim
    // preservation of the CONTAINER and its other children, exactly the
    // SElementKind::Container policy a track or a mixer already gets.
    int nChildren = 0;
    SLink *knownChild = nullptr;
    for( SLink *lk : fragment->childLinks() ) {
        ++nChildren;
        if( lk && dynamic_cast<SMidiCut *>( &lk->getSObject() ) ) knownChild = lk;
    }
    check( nChildren == 1,
           "AC1.3: only the RESOLVABLE child survives (the dangling one was "
           "dropped, not the whole fragment)" );
    check( knownChild != nullptr,
           "AC1.3: the known child (the SMidiCut) is exactly the survivor" );
    if( knownChild ) {
        check( knownChild->getStartTime() == 500,
               "AC1.3: the surviving child's placement (startTime) round-trips" );
    }

    SLink *cutLink = loader.getObjectDictionary().value( QStringLiteral( "cut1" ) );
    check( cutLink != nullptr, "the SCut windowing the fragment round-trips" );
    if( cutLink ) {
        SCut *cut = dynamic_cast<SCut *>( &cutLink->getSObject() );
        check( cut != nullptr, "...as an SCut" );
        if( cut ) {
            check( &cut->getContent() == fragment,
                   "AC1.3/1.4: the reloaded SCut still windows the reloaded "
                   "fragment (content link resolved through the object "
                   "dictionary, same as any other content type)" );
        }
    }
}

}  // namespace

// The partition rule pack-selection is built on, and the SAME function the
// arranger's "Pack clips into fragment" menu item reads to decide whether to
// enable itself. It is purely syntactic -- a clip path's lane is its path
// minus the last element -- so it needs no project, which is exactly why it
// can be shared by a menu that must answer instantly and an action that must
// answer correctly.
//
// This is the closest thing to a gate the MENU has: no testkit verb in this
// repo can open a context menu, so what is pinned here is the DECISION the
// item makes, not the item. The action's own end-to-end behaviour is
// qxa.fragment_pack_selection.
static void testPackSelectionPartition()
{
    using namespace spackselection;
    auto P = []( std::initializer_list<int> v ) { return QList<int>( v ); };

    // Three lanes: 3 clips, 2 clips, 1 clip. Two lanes are packable.
    const QList<QList<int>> mixed = {
        P({0,0}), P({0,1}), P({0,2}), P({1,0}), P({1,1}), P({2,0}) };
    check( groupByLane( mixed ).size() == 3,
           "groupByLane: three lanes in the selection" );
    check( packableLaneCount( mixed ) == 2,
           "packableLaneCount: the two lanes holding 2+ clips" );

    // Lanes come out in FIRST-SEEN order and each keeps its clips' order,
    // which is what makes a composite's member order stable run to run.
    const QList<QList<int>> interleaved = {
        P({1,0}), P({0,0}), P({1,1}), P({0,1}) };
    const QList<QList<QList<int>>> g = groupByLane( interleaved );
    check( g.size() == 2 && g[0].size() == 2 && g[1].size() == 2,
           "groupByLane: interleaved paths regroup into two lanes" );
    check( g[0][0] == P({1,0}) && g[0][1] == P({1,1}),
           "groupByLane: first-seen lane first, its clips in order" );

    // Nothing to pack: every lane holds exactly one clip. The action REFUSES
    // on this, rather than applying an empty composite.
    check( packableLaneCount( { P({0,0}), P({1,0}), P({2,0}) } ) == 0,
           "packableLaneCount: singletons only => nothing to pack" );
    check( packableLaneCount( {} ) == 0,
           "packableLaneCount: empty selection" );

    // A NESTED lane is its own lane: "0,1,0" and "0,1,1" share the container
    // path "0,1", and neither joins top-level lane "0".
    check( packableLaneCount( { P({0,0}), P({0,1,0}), P({0,1,1}) } ) == 1,
           "packableLaneCount: a nested lane groups by its own full path" );

    // Grouping is SYNTACTIC and stays so: a ONE-element path is a clip on the
    // ROOT container, so its lane path is the EMPTY one, and it forms a group
    // of its own rather than joining lane "0". That is deliberate -- deciding
    // here that such a path is really a lane would mean resolving it, which
    // this function must not do to stay callable from a menu. Whether it
    // names a clip at all is pack-clips' business: placementAt() returns null
    // for a nested track, and the member refuses, rolling the composite back.
    check( groupByLane( { P({0}), P({0,0}), P({0,1}) } ).size() == 2,
           "groupByLane: a root-level path is its own group, not lane 0's" );

    // The one path shape that IS dropped here: an empty one. The action
    // refuses on it up front, before any grouping.
    check( groupByLane( { QList<int>(), P({0,0}), P({0,1}) } ).size() == 1,
           "groupByLane: an empty path contributes no group" );
}

int main( int argc, char **argv )
{
    // QApplication (offscreen) for the app object libraries' static
    // registrars, like project_channels_test / action_roundtrip_test.
    QApplication app( argc, argv );

    StubAppContext ctx;
    SAppContext::setInstance( &ctx );

    {
        std::unique_ptr<SProject> p( new SProject );
        p->setChannels( 1 );   // keeps the fixture components (mono) and the
                                // fragment's twTrackMix width in agreement —
                                // proposal 36 B4 discipline, sidestepped
                                // rather than exercised here (that is B4's
                                // own gate, not this milestone's).

        testStructuralPredicates( *p );
        testUnitySumAndNoOwnProcessing( *p );
        testLaneStateNeverLeaksFromFragment( *p );
        testCutWindowsFragmentLikeTrack( *p );
        testAssetNameAddressing( *p );
        testPackSelectionPartition();
    }

    testSerializeRoundTrip();

    if( g_failures ) {
        std::printf( "\n%d check(s) FAILED\n", g_failures );
        return 1;
    }
    std::printf( "\nall checks passed\n" );
    return 0;
}
