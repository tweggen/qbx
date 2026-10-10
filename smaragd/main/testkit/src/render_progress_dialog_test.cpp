// Gate for QBX-145 — the render progress dialog POLLS; it does not take over
// the session's callbacks.
//
// SRenderProgressDialog used to assign session->onProgress / onComplete in its
// constructor, which ran AFTER SApplication::startRender had installed its own
// onComplete (resume the scheduler's background lane, re-arm the live monitor)
// and after the render thread was already running. So every GUI export left
// the background lane paused and the live monitor suspended until restart, and
// the assignment raced the render thread's reads of those very members. The
// fix makes the dialog a pure poller (THREADING.md rule 1, render/CONTRACT.md
// inv. 3 and 9).
//
// What this pins, against a real SProject's root rendered to a temp WAV:
//
//   1. THE CALLER'S onComplete STILL RUNS. It is installed before start(), the
//      dialog is constructed after it — the GUI's order — and the callback
//      must fire. On the unfixed dialog it never does: the dialog replaced it.
//   2. THE DIALOG NOTICES COMPLETION BY ITSELF: once the session stops running
//      and one more timer tick has passed, the button reads "Close".
//   3. A FAILED render (cancelled) ends the dialog with "Error: ...", still
//      with "Close".
//   4. A start() that cannot open its file returns false with the writer's
//      reason — the text SAppContext::startRender now propagates.
//
// Same shape as project_channels_test: a real SProject against a
// StubAppContext, offscreen, no audio device.

#include "app/model/sappcontext.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/servicesui/srenderprogress.h"

#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"
#include "tw/render/render_session.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
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

class StubAppContext : public SAppContext {
public:
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

// Pump the event loop until the session stops running, then for one more
// update-timer period (100 ms) plus margin, so the dialog's poll has run.
bool pumpUntilDone( audio::RenderSession &session, int timeoutMs )
{
    QElapsedTimer t;
    t.start();
    while( session.isRunning() ) {
        if( t.elapsed() > timeoutMs ) return false;
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
        QThread::msleep( 5 );
    }
    QElapsedTimer tail;
    tail.start();
    while( tail.elapsed() < 350 ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
        QThread::msleep( 5 );
    }
    return true;
}

QPushButton *closeButton( SRenderProgressDialog &dlg )
{
    // The dialog has exactly one button (Cancel, which becomes Close).
    return dlg.findChild<QPushButton *>();
}

bool anyLabelContains( SRenderProgressDialog &dlg, const QString &text )
{
    for( QLabel *l : dlg.findChildren<QLabel *>() )
        if( l->text().contains( text ) ) return true;
    return false;
}

void testSuccessKeepsCallersOnComplete( const std::shared_ptr<twComponent> &root,
                                        const QString &dir )
{
    audio::RenderSession session;

    // Installed BEFORE start(), as SApplication::startRender and
    // SFeelFlowBounce do. This is what the old dialog overwrote.
    std::atomic<bool> completed{ false };
    std::atomic<bool> completedOk{ false };
    session.onComplete = [&]( bool ok, const char * ) {
        completedOk = ok;
        completed = true;
    };

    audio::RenderParams params;
    params.outputPath = QDir( dir ).filePath( QStringLiteral("ok.wav") ).toStdString();
    params.format = audio::AudioFormat::WAV;
    params.startTimeSec = 0.0;
    params.endTimeSec = 0.25;

    const bool started = session.start( root, params, 48000 );
    check( started, "a 0.25 s render of the project root starts" );
    if( !started ) {
        std::printf( "     start() said: %s\n", session.errorMessage() );
        return;
    }

    // The GUI's order: the dialog is constructed after start().
    SRenderProgressDialog dlg( &session, QString::fromStdString( params.outputPath ) );

    check( pumpUntilDone( session, 60000 ), "the render finishes" );
    check( completed.load(),
           "QBX-145: the CALLER's onComplete ran (the dialog no longer replaces it)" );
    check( completedOk.load(), "...and reported success" );

    QPushButton *b = closeButton( dlg );
    check( b && b->text() == QStringLiteral("Close"),
           "the dialog noticed completion by polling: the button reads Close" );
    check( anyLabelContains( dlg, QStringLiteral("Render complete") ),
           "...and says the render is complete" );
}

void testCancelledRenderReportsFailure( const std::shared_ptr<twComponent> &root,
                                        const QString &dir )
{
    audio::RenderSession session;
    std::atomic<bool> completed{ false };
    session.onComplete = [&]( bool, const char * ) { completed = true; };

    audio::RenderParams params;
    params.outputPath = QDir( dir ).filePath( QStringLiteral("cancel.wav") ).toStdString();
    params.format = audio::AudioFormat::WAV;
    params.startTimeSec = 0.0;
    params.endTimeSec = 600.0;   // long enough that the cancel is always observed

    const bool started = session.start( root, params, 48000 );
    check( started, "a long render starts" );
    if( !started ) return;

    SRenderProgressDialog dlg( &session, QString::fromStdString( params.outputPath ) );
    session.requestCancel();

    check( pumpUntilDone( session, 60000 ), "the cancelled render ends" );
    check( completed.load(), "the caller's onComplete ran for the cancelled render" );

    QPushButton *b = closeButton( dlg );
    check( b && b->text() == QStringLiteral("Close"),
           "a failed render also ends with Close" );
    check( anyLabelContains( dlg, QStringLiteral("Error: Render cancelled") ),
           "...and the dialog shows the session's error text" );
}

void testStartFailureCarriesTheReason( const std::shared_ptr<twComponent> &root,
                                       const QString &dir )
{
    audio::RenderSession session;
    audio::RenderParams params;
    // A directory that does not exist: the writer cannot open the file.
    params.outputPath = QDir( dir ).filePath(
        QStringLiteral("no/such/dir/out.wav") ).toStdString();
    params.format = audio::AudioFormat::WAV;
    params.startTimeSec = 0.0;
    params.endTimeSec = 0.25;

    check( !session.start( root, params, 48000 ),
           "start() refuses a file it cannot open" );
    check( !session.isRunning(), "...and nothing runs" );
    check( QString::fromUtf8( session.errorMessage() )
               .startsWith( QStringLiteral("Failed to open output file") ),
           "...and errorMessage() says why" );
}

}  // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );

    StubAppContext ctx;
    SAppContext::setInstance( &ctx );

    QTemporaryDir tmp;
    check( tmp.isValid(), "a temp directory for the renders" );

    {
        // The root a new project gets (SActionRunner, SMainWindow::newProject):
        // an empty SStdMixer, owned by the project.
        std::unique_ptr<SProject> project( new SProject );
        project->setRootComponent( new SStdMixer( project.get() ) );
        SObject *rootObj = project->getRootComponent();
        std::shared_ptr<twComponent> root =
            rootObj ? rootObj->getRootComponent() : nullptr;
        check( root != nullptr, "the project's root mixer has a component" );
        if( root ) {
            testSuccessKeepsCallersOnComplete( root, tmp.path() );
            testCancelledRenderReportsFailure( root, tmp.path() );
            testStartFailureCarriesTheReason( root, tmp.path() );
        }
    }

    if( g_failures ) {
        std::printf( "\n%d check(s) FAILED\n", g_failures );
        return 1;
    }
    std::printf( "\nall checks passed\n" );
    return 0;
}
