// Gate for proposal 35 M1 — the project's channel count as PROJECT DATA.
//
// M1 adds one integer to SProject, one attribute to the .qxp, and one action.
// Its whole risk is in the negative space around that, so this is what the test
// pins:
//
//   1. THE DEFAULT IS 2, for a fresh project and for a legacy file with no
//      channels= attribute — 2 is what every project written before this
//      attribute existed sounds like, so any other default silently rewrites
//      history.
//   2. THE LEGACY LOAD WARNS EXACTLY ONCE. Not zero (a wrong channel count
//      corrupts a project as thoroughly as a wrong sample rate, which is why
//      the sampleRate idiom warns), and not once per something.
//   3. AN UNSUPPORTED WIDTH NEVER COSTS THE DOCUMENT. 3, 16, "abc" all load as
//      2 with a warning; the project opens.
//   4. THE ATTRIBUTE SURVIVES A ROUND TRIP, including the resave: serialize ->
//      parse -> serialize is identical for every supported width.
//   5. NOTHING PROPAGATES TO A BUS COUNT. This is AC 1.4 and the reason
//      STrack::setNBussesCallCount() exists: applying set-project-channels and
//      then its inverse must move that counter by ZERO. Reading the code proves
//      it today; the counter proves it after the next person edits it.
//
// The .qxp-level evidence (nested tracks, save/load/save, a byte-identical
// render across the action) is in tests/cases/project_channels_roundtrip.qxa —
// this binary covers what a script cannot see.

#include "app/actions/saction.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sappcontext.h"
#include "app/model/sproject.h"
#include "app/objects/track/strack.h"

#include "tw/graph/tw303aenv.h"

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QTextStream>
#include <QtGlobal>

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

// --- warning counting ----------------------------------------------------
// Only warnings whose text mentions the channel count are counted: a load also
// warns about other things (the sample rate, on the same legacy file), and a
// test that counted every warning would be measuring the wrong thing.
int g_channelWarnings = 0;
int g_busWarnings = 0;
QtMessageHandler g_prevHandler = nullptr;

void countingHandler( QtMsgType type, const QMessageLogContext &ctx, const QString &msg )
{
    if( type == QtWarningMsg ) {
        if( msg.contains( QStringLiteral("channels"), Qt::CaseInsensitive ) ) {
            ++g_channelWarnings;
        }
        if( msg.contains( QStringLiteral("busses"), Qt::CaseInsensitive ) ) {
            ++g_busWarnings;
        }
    }
    if( g_prevHandler ) g_prevHandler( type, ctx, msg );
}

// The narrow app context the model layer asserts on (SAppContext::get()).
// STrack's construction reaches it, so a test that builds a real track has to
// stand one up; everything here is inert, because nothing this test does wants
// a rendered anything.
class StubAppContext : public SAppContext {
public:
    // STrack::setNBusses builds twTrackMix / twPluginChain / twRewire against
    // the environment, so this one is REAL. Everything else is inert.
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

private:
    mutable tw303aEnvironment env_;
};

// Parse an <SProject …> element out of a literal and feed it to the reader,
// which is exactly what SProjectLoader does with the real document.
void loadAttributes( SProject &p, const char *xml )
{
    QDomDocument doc;
    doc.setContent( QString::fromLatin1( xml ) );
    QDomElement el = doc.documentElement();
    p.readPreChildrenAttributes( el );
}

QString selfAttributes( SProject &p )
{
    QString s;
    QTextStream ts( &s );
    p.serializeSelfAttributes( ts );
    ts.flush();
    return s;
}

// An SProject costs a 512 MB CapturePagePool, so this file keeps at most ONE
// alive at a time: every helper takes its project by reference and main() hands
// out one at a time from a unique_ptr it resets between phases.
void testDefaults( SProject &p )
{
    check( p.channels() == 2, "a fresh project is 2 channels" );
    check( selfAttributes( p ).contains( QStringLiteral(" channels='2'") ),
           "a fresh project serializes channels='2'" );
}

void testValidWidths( SProject &p )
{
    check(  SProject::isValidChannelCount( 1 ), "1 is a valid width" );
    check(  SProject::isValidChannelCount( 2 ), "2 is a valid width" );
    check(  SProject::isValidChannelCount( 4 ), "4 is a valid width" );
    check(  SProject::isValidChannelCount( 6 ), "6 is a valid width" );
    check(  SProject::isValidChannelCount( 8 ), "8 is a valid width" );
    check( !SProject::isValidChannelCount( 0 ), "0 is refused" );
    check( !SProject::isValidChannelCount( 3 ), "3 is refused" );
    check( !SProject::isValidChannelCount( 5 ), "5 is refused" );
    check( !SProject::isValidChannelCount( 7 ), "7 is refused" );
    check( !SProject::isValidChannelCount( 16 ), "16 is refused" );
    check( !SProject::isValidChannelCount( -2 ), "a negative width is refused" );

    p.setChannels( 3 );
    check( p.channels() == 2, "setChannels(3) is refused, not clamped to 2 by luck" );
    p.setChannels( 6 );
    check( p.channels() == 6, "setChannels(6) takes" );
    p.setChannels( 0 );
    check( p.channels() == 6, "setChannels(0) leaves the previous width alone" );
}

// AC 1.2: a legacy .qxp without channels= loads as 2 and warns exactly once.
void testLegacyLoad( SProject &p )
{
    g_channelWarnings = 0;
    loadAttributes( p,
        "<SProject bpmTempo='120' sampleRate='48000' posFactor='1/48000'/>" );
    check( p.channels() == 2, "AC 1.2: a legacy project loads as 2 channels" );
    check( g_channelWarnings == 1,
           "AC 1.2: a legacy project warns about channels exactly once" );

    // Not 2 because it started at 2: a project that already carries 6 must be
    // brought BACK to 2 by a legacy document, or a reused project object would
    // keep a width the file does not describe.
    p.setChannels( 6 );
    g_channelWarnings = 0;
    loadAttributes( p,
        "<SProject bpmTempo='120' sampleRate='48000'/>" );
    check( p.channels() == 2, "a legacy load resets an already-wide project to 2" );
    check( g_channelWarnings == 1, "…and still warns exactly once" );
}

void testBadValueNeverCostsTheDocument( SProject &p )
{
    const char *bad[] = {
        "<SProject sampleRate='48000' channels='3'/>",
        "<SProject sampleRate='48000' channels='16'/>",
        "<SProject sampleRate='48000' channels='0'/>",
        "<SProject sampleRate='48000' channels='abc'/>",
        "<SProject sampleRate='48000' channels='-2'/>",
    };
    for( const char *xml : bad ) {
        p.setChannels( 6 );   // so the fallback to 2 is a real change
        g_channelWarnings = 0;
        loadAttributes( p, xml );
        check( p.channels() == 2, "an unsupported channels= falls back to 2" );
        check( g_channelWarnings == 1, "…and warns exactly once" );
    }
}

// AC 1.1 / AC 1.3 at the attribute level: serialize -> parse -> serialize is
// identical, for every supported width. (The whole-document save/load/save is
// project_channels_roundtrip.qxa's job; this pins the one attribute so a
// failure localises here instead of in a 200-line diff.)
void testRoundTrip( SProject &p )
{
    const int widths[] = { 1, 2, 4, 6, 8 };
    for( int w : widths ) {
        p.setChannels( w );
        const QString first = selfAttributes( p );

        // Reload the very attributes just written, into the same object: the
        // fixed point is what matters, and a second SProject would only add
        // another half-gigabyte of page pool to prove the same thing.
        const QString doc = QStringLiteral("<SProject%1/>").arg( first );
        g_channelWarnings = 0;
        loadAttributes( p, doc.toLatin1().constData() );
        check( p.channels() == w, "a written width parses back unchanged" );
        check( g_channelWarnings == 0,
               "a project WITH channels= warns about them not at all" );

        const QString second = selfAttributes( p );
        check( first == second, "serialize -> parse -> serialize is identical" );
    }
    p.setChannels( 2 );
}

// AC 1.4: set-project-channels and its undo touch no setNBusses call.
void testActionTouchesNoBusCount( SProject &project )
{
    SActionRegistry &registry = SActionRegistry::instance();

    // A real track, so there IS a bus count to disturb. Its constructor calls
    // setNBusses(2) — which is why the baseline is taken AFTER it exists.
    std::unique_ptr<STrack> track( new STrack( &project ) );
    check( track->getNBusses() == 2, "a fresh STrack has 2 busses" );

    const long baseline = STrack::setNBussesCallCount();
    check( baseline > 0, "the setNBusses counter is live (the ctor moved it)" );

    std::unique_ptr<SAction> act(
        registry.create( QStringLiteral("set-project-channels") ) );
    check( act != nullptr, "set-project-channels is registered" );
    if( !act ) return;

    QDomDocument doc;
    QDomElement el = doc.createElement( QStringLiteral("set-project-channels") );
    el.setAttribute( QStringLiteral("channels"), 6 );
    check( act->readXml( el, act->formatVersion() ), "…and reads its own XML" );

    SApplyResult res = act->apply( &project );
    check( res.applied, "AC 1.4: applying 6 channels succeeds" );
    check( project.channels() == 6, "AC 1.4: the project is 6 channels" );
    check( res.inverse != nullptr, "AC 1.4: the action is undoable" );

    if( res.inverse ) {
        SApplyResult undone = res.inverse->apply( &project );
        check( undone.applied, "AC 1.4: the undo applies" );
        check( project.channels() == 2, "AC 1.4: the undo restores 2 channels" );
        delete undone.inverse;
        delete res.inverse;
    }

    // THE ASSERTION THIS FILE EXISTS FOR. 6 -> 2 is a SHRINK, and
    // STrack::setNBusses refuses a shrink with Q_ASSERT_X( false, ... ): had the
    // undo reached it, a Debug build would have aborted above and this build
    // (RelWithDebInfo, where Qt's QT_NO_DEBUG compiles Q_ASSERT_X out) would
    // have gone on with a half-wired track and said nothing. Zero calls is the
    // only answer that means "M1 is inaudible".
    check( STrack::setNBussesCallCount() == baseline,
           "AC 1.4: apply + undo made NO setNBusses call" );
    check( track->getNBusses() == 2, "AC 1.4: the track is still 2 busses" );
}

void testActionRejectsUnsupportedWidth( SProject &project )
{
    SActionRegistry &registry = SActionRegistry::instance();

    for( int bad : { 3, 5, 16, 0 } ) {
        std::unique_ptr<SAction> act(
            registry.create( QStringLiteral("set-project-channels") ) );
        QDomDocument doc;
        QDomElement el = doc.createElement( QStringLiteral("set-project-channels") );
        el.setAttribute( QStringLiteral("channels"), bad );
        act->readXml( el, act->formatVersion() );

        SApplyResult res = act->apply( &project );
        check( !res.applied, "an unsupported width is REJECTED by the action" );
        check( res.inverse == nullptr, "…and hands the undo stack nothing" );
        check( project.channels() == 2, "…and leaves the project alone" );
    }

    // A no-op request is APPLIED (the requested state is the state) but carries
    // no inverse: an undo step that changes nothing is noise on the stack.
    std::unique_ptr<SAction> same(
        registry.create( QStringLiteral("set-project-channels") ) );
    QDomDocument doc;
    QDomElement el = doc.createElement( QStringLiteral("set-project-channels") );
    el.setAttribute( QStringLiteral("channels"), 2 );
    same->readXml( el, same->formatVersion() );
    SApplyResult res = same->apply( &project );
    check( res.applied, "setting the width it already has is applied" );
    check( res.inverse == nullptr, "…with no undo step" );
}

// The other half of the M1 fix: STrack's loader default used to be "1" while
// its constructor builds 2, so a .qxp that omitted nBusses (or carried 1) asked
// for a shrink and hit the same Q_ASSERT_X.
void testTrackBusCountLoad( SProject &project )
{
    // The WARNING is the discriminator, not the resulting count. Q_ASSERT_X is
    // compiled out under QT_NO_DEBUG — which this project's default
    // RelWithDebInfo build defines, even though it strips NDEBUG to keep its own
    // asserts — so the OLD code did not abort here: it hit the shrink branch and
    // returned SILENTLY, leaving 2 busses. The count was therefore right by
    // accident and would have stayed right through any regression. What is new
    // is that a request the loader cannot honour now SAYS so, exactly once.
    struct Case { const char *xml; int wantBusses; int wantWarnings; const char *what; };
    const Case cases[] = {
        { "<STrack/>", 2, 0,
          "a track with no nBusses= loads at the ctor's width, silently" },
        { "<STrack nBusses='1'/>", 2, 1,
          "a track asking to SHRINK to 1 keeps 2 and warns exactly once" },
        { "<STrack nBusses='2'/>", 2, 0,
          "a track asking for 2 (what every real fixture writes) is silent" },
        { "<STrack nBusses='4'/>", 4, 0,
          "a track can still GROW on load" },
    };
    for( const Case &c : cases ) {
        std::unique_ptr<STrack> t( new STrack( &project ) );
        QDomDocument doc;
        doc.setContent( QString::fromLatin1( c.xml ) );
        QDomElement el = doc.documentElement();
        g_busWarnings = 0;
        t->readPreChildrenAttributes( el );
        check( t->getNBusses() == c.wantBusses && g_busWarnings == c.wantWarnings,
               c.what );
    }
}

}  // namespace

int main( int argc, char **argv )
{
    // QApplication (offscreen) for the app object libraries' static registrars,
    // like action_roundtrip_test and preview_container_test.
    QApplication app( argc, argv );

    StubAppContext ctx;
    SAppContext::setInstance( &ctx );

    g_prevHandler = qInstallMessageHandler( countingHandler );

    {
        std::unique_ptr<SProject> p( new SProject );
        testDefaults( *p );
        testValidWidths( *p );
        p->setChannels( 2 );
        testLegacyLoad( *p );
        testBadValueNeverCostsTheDocument( *p );
        testRoundTrip( *p );
    }
    {
        // A fresh project for the action phase: the counter assertion wants a
        // project whose width is the default 2, so that 6 -> 2 really is the
        // shrink the undo has to survive.
        std::unique_ptr<SProject> p( new SProject );
        testActionTouchesNoBusCount( *p );
        testActionRejectsUnsupportedWidth( *p );
        testTrackBusCountLoad( *p );
    }

    qInstallMessageHandler( g_prevHandler );

    if( g_failures ) {
        std::printf( "\n%d check(s) FAILED\n", g_failures );
        return 1;
    }
    std::printf( "\nall checks passed\n" );
    return 0;
}
