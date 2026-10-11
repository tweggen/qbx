// Gate for QBX-144 -- the render dialog's output defaults to the PROJECT's
// directory, and a relative entry resolves against it.
//
// The dialog used to default its output to the bare relative "render.wav" and
// pass the line edit's text through unchanged, so the writer resolved it
// against the process's working directory: renders landed wherever Smaragd was
// started from (the repo root, for a dev build). Browse... opened with no
// starting path at all.
//
// What this pins, against a real SProject with an SStdMixer root:
//
//   a. A saved project <dir>/song.qxp defaults the output to <dir>/song.wav.
//   b. Switching to OGG swaps the suffix of that ABSOLUTE path: <dir>/song.ogg.
//   c. A relative entry ("take2.wav") renders to <dir>/take2.wav: what
//      getRenderParams() hands the render, and what resolvedOutputPath()
//      returns -- the latter is exactly what Browse... opens on (the file
//      dialog itself cannot be driven headlessly).
//   d. An untitled project defaults under Documents, absolute and clean.
//   e. A non-ASCII project directory survives: default, resolved and the UTF-8
//      outputPath all keep the characters.
//
// Same shape as render_progress_dialog_test: a real SProject against a
// StubAppContext, offscreen, no audio device.

#include "app/model/sappcontext.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/servicesui/srenderdialog.h"

#include "tw/graph/tw303aenv.h"
#include "tw/render/render_session.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QRadioButton>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

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

void checkEq( const QString &got, const QString &want, const char *what )
{
    check( got == want, what );
    if( got != want )
        std::printf( "     got  \"%s\"\n     want \"%s\"\n",
                     got.toUtf8().constData(), want.toUtf8().constData() );
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
    bool startRender( const audio::RenderParams &, QString * ) override { return false; }
    bool isRenderingActive() const override { return false; }
    void setPlaybackRunning( bool ) override {}
    offset_t getGlobalLocatorPos() const override { return 0; }

private:
    mutable tw303aEnvironment env_;
};

std::unique_ptr<SProject> makeProject( const QString &projectFile )
{
    // The root a new project gets (SActionRunner, SMainWindow::newProject).
    std::unique_ptr<SProject> project( new SProject );
    project->setRootComponent( new SStdMixer( project.get() ) );
    if( !projectFile.isEmpty() ) project->setProjectFilePath( projectFile );
    return project;
}

QLineEdit *pathEdit( SRenderDialog &dlg )
{
    // The dialog has exactly one line edit: the output path.
    return dlg.findChild<QLineEdit *>();
}

QRadioButton *radio( SRenderDialog &dlg, const QString &text )
{
    for( QRadioButton *r : dlg.findChildren<QRadioButton *>() )
        if( r->text() == text ) return r;
    return nullptr;
}

QString outputPathOf( const SRenderDialog &dlg )
{
    return QString::fromStdString( dlg.getRenderParams().outputPath );
}

void testSavedProject( const QString &dir )
{
    std::unique_ptr<SProject> project = makeProject( dir + "/song.qxp" );
    SRenderDialog dlg( project.get() );
    QLineEdit *edit = pathEdit( dlg );
    check( edit != nullptr, "the dialog has an output line edit" );
    if( !edit ) return;

    // a.
    checkEq( edit->text(), dir + "/song.wav",
             "a. the default output is <projectdir>/<projectname>.wav" );

    // b.
    QRadioButton *ogg = radio( dlg, QStringLiteral( "OGG Vorbis" ) );
    check( ogg != nullptr, "b. there is an OGG radio button" );
    if( ogg ) {
        ogg->click();
        checkEq( edit->text(), dir + "/song.ogg",
                 "b. switching to OGG swaps the suffix of the absolute path" );
        QRadioButton *wav = radio( dlg, QStringLiteral( "WAV (PCM 16-bit)" ) );
        if( wav ) wav->click();
    }

    // c.
    edit->setText( QStringLiteral( "take2.wav" ) );
    checkEq( outputPathOf( dlg ), dir + "/take2.wav",
             "c. a relative entry renders into the project directory" );
    checkEq( dlg.resolvedOutputPath(), dir + "/take2.wav",
             "c. ...and resolvedOutputPath() (what Browse... opens on) agrees" );

    edit->setText( QStringLiteral( "sub/../mix.wav" ) );
    checkEq( dlg.resolvedOutputPath(), dir + "/mix.wav",
             "c. a relative entry is cleaned" );

    const QString elsewhere = QDir::cleanPath( QDir::tempPath() ) + "/elsewhere.wav";
    edit->setText( elsewhere );
    checkEq( outputPathOf( dlg ), elsewhere,
             "c. an absolute entry passes through unchanged" );
}

void testUntitledProject()
{
    std::unique_ptr<SProject> project = makeProject( QString() );
    check( project->projectDir().isEmpty(), "d. an untitled project has no projectDir()" );

    SRenderDialog dlg( project.get() );
    QLineEdit *edit = pathEdit( dlg );
    if( !edit ) { check( false, "d. the dialog has an output line edit" ); return; }

    const QString docs = QDir::cleanPath(
        QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation ) );
    const QString text = edit->text();
    check( QDir::isAbsolutePath( text ), "d. the untitled default is absolute" );
    checkEq( text, docs + "/render.wav", "d. ...and is Documents/render.wav" );
    checkEq( QDir::cleanPath( text ), text, "d. ...with no relative component" );

    edit->setText( QStringLiteral( "take2.wav" ) );
    checkEq( outputPathOf( dlg ), docs + "/take2.wav",
             "d. a relative entry in an untitled project resolves under Documents" );
}

void testNonAsciiProjectDir( const QString &tmp )
{
    const QString dir = tmp + QString::fromUtf8( "/Prinzessin_\xC3\xA4" );
    QDir().mkpath( dir );
    std::unique_ptr<SProject> project = makeProject( dir + "/song.qxp" );
    checkEq( project->projectDir(), dir, "e. projectDir() keeps the non-ASCII characters" );

    SRenderDialog dlg( project.get() );
    QLineEdit *edit = pathEdit( dlg );
    if( !edit ) { check( false, "e. the dialog has an output line edit" ); return; }

    checkEq( edit->text(), dir + "/song.wav",
             "e. the default output keeps the non-ASCII directory" );
    checkEq( dlg.resolvedOutputPath(), dir + "/song.wav",
             "e. ...so does resolvedOutputPath()" );
    checkEq( outputPathOf( dlg ), dir + "/song.wav",
             "e. ...and the UTF-8 outputPath round-trips" );
}

}  // namespace

int main( int argc, char **argv )
{
    QApplication app( argc, argv );

    StubAppContext ctx;
    SAppContext::setInstance( &ctx );

    QTemporaryDir tmp;
    check( tmp.isValid(), "a temp directory to put the projects in" );
    const QString dir = QDir::cleanPath( QFileInfo( tmp.path() ).absoluteFilePath() );

    testSavedProject( dir );
    testUntitledProject();
    testNonAsciiProjectDir( dir );

    if( g_failures ) {
        std::printf( "\n%d check(s) FAILED\n", g_failures );
        return 1;
    }
    std::printf( "\nall checks passed\n" );
    return 0;
}
