
#include <qapplication.h>
#include <QFontDatabase>
#include <QFont>
#include <QCommandLineParser>
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "app/testkit/sactionscript.h"
#include "app/testkit/sactionrunner.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/ssettings.h"
#include "app/servicesui/soptions.h"
#include "tw/core/twlog.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#ifdef Q_OS_MACOS
#include <ApplicationServices/ApplicationServices.h>   // TransformProcessType
#endif

// Route Qt's qDebug/qInfo/qWarning/qCritical/qFatal into the TwLog sink.
//
// This is not merely re-plumbing. Qt's DEFAULT handler on Windows/MinGW writes
// to the Windows debug channel, not stderr, which is why these messages never
// reached a redirected log while the engine's syslog() output did — a defect
// recorded twice in plan/STATE.md, most recently as "qWarning() output is
// invisible in this Windows/MinGW build, so action-level diagnostics do not
// reach the test log". Installing this handler replaces that default outright,
// so every Qt message now leaves through the same file descriptor as the rest.
static void smaragdMessageHandler( QtMsgType type,
                                   const QMessageLogContext &ctx,
                                   const QString &msg )
{
    tw::LogLevel level;
    switch( type ) {
        case QtDebugMsg:    level = tw::LogLevel::Debug; break;
        case QtInfoMsg:     level = tw::LogLevel::Info;  break;
        case QtWarningMsg:  level = tw::LogLevel::Warn;  break;
        case QtCriticalMsg:
        case QtFatalMsg:
        default:            level = tw::LogLevel::Error; break;
    }

    // Qt's category is "default" for a bare qDebug(); prefix ours so the dock's
    // category filter separates Qt traffic from the engine's module categories.
    const char *cat = "qt";
    QByteArray catBuf;
    if( ctx.category && *ctx.category && qstrcmp( ctx.category, "default" ) != 0 ) {
        catBuf = QByteArray( "qt." ) + ctx.category;
        cat = catBuf.constData();
    }

    tw::TwLog::instance().logStr( level, cat, ctx.file, ctx.line,
                                  msg.toStdString() );

    // qFatal() must still abort — but not before the record reaches the file.
    if( type == QtFatalMsg ) {
        tw::TwLog::instance().shutdown();
        abort();
    }
}

// Parse a level name; returns false if `s` names no level.
static bool parseLogLevel( const QString &s, tw::LogLevel &out )
{
    const QString v = s.trimmed().toLower();
    if( v == "error" || v == "err" )  { out = tw::LogLevel::Error; return true; }
    if( v == "warn"  || v == "warning" ) { out = tw::LogLevel::Warn; return true; }
    if( v == "info" )                 { out = tw::LogLevel::Info;  return true; }
    if( v == "debug" )                { out = tw::LogLevel::Debug; return true; }
    if( v == "trace" )                { out = tw::LogLevel::Trace; return true; }
    return false;
}

int main( int argc, char *argv[] )
{
    // The log sink comes up FIRST, before QApplication, so nothing in startup is
    // lost. The ring and the console tee are live immediately; the rotating file
    // is attached further down, once the config directory is known.
    qInstallMessageHandler( smaragdMessageHandler );
    tw::TwLog::nameThread( "gui" );
    tw::TwLog::instance().setConsole( SMARAGD_LOG_CONSOLE_DEFAULT ? true : false );

    // Detect headless test mode BEFORE QApplication is constructed. This has to
    // happen here and not one line later: the platform integration is built by
    // the QApplication constructor, and on macOS that is precisely where the
    // process is promoted to a foreground app (see below).
    bool headlessMode = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--test-case") == 0) {
            headlessMode = true;
            break;
        }
    }

    if (headlessMode) {
        // A headless case gets the CAPTURE backend by default: it pumps the
        // render callback on a real-time paced clock and keeps every frame, so
        // playback is observable (dump-playback-capture) instead of merely
        // "started". The platform backend is the wrong default here twice over —
        // a ctest run would open the real output device ~90 times and make noise
        // on the developer's machine, and its callback timing is at the mercy of
        // whatever else is playing.
        //
        // This must be set BEFORE SApplication is constructed: the speaker (and
        // with it the backend) is minted in that constructor, and createAudioBackend()
        // reads the variable exactly once, there.
        //
        // Only when unset — an explicit SMARAGD_AUDIO_BACKEND always wins, which
        // is how a case can still be run against the real device by hand.
        if (qEnvironmentVariableIsEmpty("SMARAGD_AUDIO_BACKEND"))
            qputenv("SMARAGD_AUDIO_BACKEND", "capture");

#ifdef Q_OS_LINUX
        // Same intent as the previous argv rewrite, minus the undefined
        // behaviour: that version built `new char*[argc + 2]`, filled every slot
        // and never wrote the argv[argc] == nullptr terminator the C standard
        // requires (it also leaked the array, and handed a heap array to a
        // QApplication that keeps the reference for the process lifetime).
        // qputenv reaches the platform selection the same way with none of that.
        //
        // The old code skipped itself when `-platform` appeared in argv; the env
        // var needs no such check, because an explicit `-platform` on the command
        // line outranks QT_QPA_PLATFORM in Qt's own resolution order. So an
        // operator override still wins, and it wins without us parsing for it.
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
            qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    }

    SApplication app( argc, argv );

#ifdef Q_OS_MACOS
    // Do not take over the desktop for a non-interactive run.
    //
    // Test mode never show()s a window (see the `!testMode && runMode` guard
    // below), yet a ctest run still puts a Smaragd tile in the Dock 106 times
    // and can pull the frontmost application away. The window is not the
    // culprit, which is why every window-level remedy is a no-op here
    // (Qt::WA_ShowWithoutActivating, Qt::WindowDoesNotAcceptFocus,
    // showMinimized(), never calling show()): they all describe a window, and
    // the process is promoted to a foreground app before one exists.
    //
    // Demote it back. This must run AFTER the QApplication constructor: that is
    // where the platform integration promotes the process, so anything set
    // earlier is simply overwritten.
    //
    // MEASURED, not assumed. Two things that look like the fix are not:
    //   - QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM=1, Qt's own gate on
    //     the promotion, changes nothing here -- sampling `background only` of
    //     the running process gives foreground=14/14 with the variable set and
    //     with it unset. For a BUNDLED .app, LaunchServices decides foreground
    //     status from the Info.plist, and Qt's gate never gets the last word.
    //   - LSUIElement / LSBackgroundOnly in the Info.plist would work, but a
    //     plist cannot be conditional: it would make the SHIPPED app dock-less.
    //
    // TransformProcessType is the one lever that is both effective and runtime,
    // so it can be applied only when --test-case is present. It is deprecated
    // but not removed, and it can only demote -- a process already promoted may
    // therefore flash in the Dock briefly before this runs.
    if (headlessMode) {
        ProcessSerialNumber psn = { 0, kCurrentProcess };
        TransformProcessType( &psn, kProcessTransformToUIElementApplication );
    }
#endif

    // Command-line parsing (Phase 1+: --run-actions, Phase 2+: --test-case, --list-actions)
    QCommandLineParser parser;
    parser.addOption({"run-actions", "Execute an action script and keep the window open.", "file"});
    parser.addOption({"test-case", "Run an action script as a headless test (exit 0 on pass, 1 on fail).", "file"});
    parser.addOption({"list-actions", "List known action verbs and exit."});
    parser.addOption(QCommandLineOption(
        {"test-output-dir", "o"},
        "Directory for test artifacts (screenshots, renders, etc.).",
        "path"
    ));
    parser.addOption({"log-console",    "Tee the log to stderr."});
    parser.addOption({"no-log-console", "Do not tee the log to stderr."});
    parser.addOption({"log-level",
        "Log threshold: error, warn, info, debug, trace.", "level"});
    parser.process(app);

    // Resolve the log configuration, last wins:
    //   compile default -> persisted option -> environment -> command line.
    // The compile default was already applied above so that startup logging had
    // somewhere to go; from here the user's preferences take over.
    {
        tw::TwLog &log = tw::TwLog::instance();
        SSettings &settings = SSettings::instance();

        bool wantConsole =
            settings.value( SOpt::LogConsole, SOpt::def( SOpt::LogConsole ) ).toBool();

        tw::LogLevel level = tw::LogLevel::Debug;
        parseLogLevel( settings.value( SOpt::LogLevel,
                                       SOpt::def( SOpt::LogLevel ) ).toString(), level );

        if( const char *env = std::getenv( "SMARAGD_LOG_CONSOLE" ) )
            wantConsole = ( env[0] == '1' || env[0] == 'y' || env[0] == 'Y' ||
                            env[0] == 't' || env[0] == 'T' );
        if( const char *env = std::getenv( "SMARAGD_LOG_LEVEL" ) )
            parseLogLevel( QString::fromUtf8( env ), level );

        if( parser.isSet( "log-console" ) )    wantConsole = true;
        if( parser.isSet( "no-log-console" ) ) wantConsole = false;
        if( parser.isSet( "log-level" ) ) {
            if( !parseLogLevel( parser.value( "log-level" ), level ) ) {
                std::cerr << "Unknown --log-level '"
                          << parser.value( "log-level" ).toStdString()
                          << "'; expected error|warn|info|debug|trace\n";
                return 2;
            }
        }

        const int cap = settings.value( SOpt::LogCapacity,
                                        SOpt::def( SOpt::LogCapacity ) ).toInt();
        size_t wantCapacity = cap > 0 ? (size_t)cap : 200000;
        // A --test-case run gets a bigger ring, and gets it in ONE call
        // (setCapacity discards whatever is already buffered). assert-log
        // reads this ring, and a case that renders for a minute can emit far
        // more records afterwards than the line it is asserting about — an
        // eviction would turn a real assertion into a silent false failure.
        // Nothing is pre-reserved per slot (twlog.cc), so the cost is
        // proportional to what actually gets logged.
        if( parser.isSet( "test-case" ) && wantCapacity < 1000000 ) {
            wantCapacity = 1000000;
        }
        log.setCapacity( wantCapacity );
        log.setConsole( wantConsole );
        log.setMinLevel( level );

        // Phase two of init: the ring has been collecting since before
        // QApplication; setFileSink starts from the oldest resident record, so
        // everything logged before this point still reaches the file.
        //
        // Headless test runs deliberately get NO file sink. The suite is 38
        // cases that would all append to the one smaragd.log in the user's
        // config directory — polluting it, and racing each other over the file
        // and its rotation the moment anyone runs `ctest -j`. Test diagnostics
        // belong on the console, which --log-console still provides.
        const bool headlessTest = parser.isSet( "test-case" );
        if( !headlessTest &&
            settings.value( SOpt::LogToFile, SOpt::def( SOpt::LogToFile ) ).toBool() )
            log.setFileSink( settings.configDir().toStdString(),
                             8u * 1024u * 1024u, 3 );

        TW_LOGI( "ui.shell", "Smaragd starting; log level=%s console=%d dir=%s",
                 tw::TwLog::levelName( level ), (int)wantConsole,
                 settings.configDir().toUtf8().constData() );
    }

    // --list-actions: output and exit before window creation
    if (parser.isSet("list-actions")) {
        QStringList names = SActionRegistry::instance().knownNames();
        names.sort();
        for (const QString &name : names) {
            std::cout << name.toStdString() << "\n";
        }
        return 0;
    }

    // Default UI font: the bundled FreeSans (embedded via resources/fonts.qrc),
    // rendered antialiased. If the font was not bundled into this build the
    // family stays the platform default; antialiasing is requested either way.
    {
        int fid = QFontDatabase::addApplicationFont(
            QStringLiteral( ":/fonts/FreeSans.ttf" ) );
        QFont uiFont = QApplication::font();
        if( fid != -1 ) {
            const QStringList fams = QFontDatabase::applicationFontFamilies( fid );
            if( !fams.isEmpty() ) uiFont.setFamily( fams.first() );
        }
        uiFont.setStyleStrategy( QFont::PreferAntialias );
        QApplication::setFont( uiFont );
    }

    // Set test output directory if provided
    QString outputDir = parser.value("test-output-dir");
    if (outputDir.isEmpty()) {
        // Try environment variable fallback
        const char *envPath = std::getenv("SMARAGD_TEST_OUTPUT_DIR");
        if (envPath) {
            outputDir = QString::fromUtf8(envPath);
        }
    }
    if (!outputDir.isEmpty()) {
        app.setTestOutputDir(outputDir);
        if (!app.ensureOutputDirExists()) {
            std::cerr << "Warning: Failed to create output directory: " << outputDir.toStdString() << "\n";
            std::cerr.flush();
        }
    }

    // Determine test mode vs interactive mode
    bool testMode = parser.isSet("test-case");
    bool runMode = parser.isSet("run-actions");
    QString scriptPath = testMode ? parser.value("test-case") : parser.value("run-actions");

    // Create main window (only shown in interactive mode)
    SMainWindow *win = new SMainWindow();
    if (!testMode && runMode) {
        // Script-driven interactive session: deterministic default window,
        // no per-user layout restore.
        win->move( 100, 100 );
        win->resize( 800, 600 );
        win->showMaximized();
    }

    // Execute action script if --run-actions or --test-case was provided
    if (runMode || testMode) {
        SActionScript script;
        if (script.readFile(scriptPath)) {
            SActionRunner runner;
            SActionRunner::Result result = runner.run(script, app);

            // Output results
            if (testMode) {
                // TAP-style output for test mode
                std::cout << (result.passed ? "PASS" : "FAIL") << " - " << scriptPath.toStdString() << "\n";
                if (!result.passed) {
                    std::cout << "# Actions applied: " << result.actionsApplied << "\n";
                    std::cout << "# Actions rejected: " << result.actionsRejected << "\n";
                    std::cout << "# Assertions failed: " << result.assertionsFailed << "\n";
                    if (!result.failures.isEmpty()) {
                        std::cout << "# Failures:\n";
                        for (const QString &failure : result.failures) {
                            std::cout << "#   - " << failure.toStdString() << "\n";
                        }
                    }
                }
                if (!result.artifacts.isEmpty()) {
                    std::cout << "# Artifacts:\n";
                    for (const QString &artifact : result.artifacts) {
                        std::cout << "#   - " << artifact.toStdString() << "\n";
                    }
                }
            } else {
                // Verbose output for run mode
                std::cout << "Script executed: " << (result.passed ? "PASS" : "FAIL") << "\n";
                std::cout << "  Actions applied: " << result.actionsApplied << "\n";
                std::cout << "  Actions rejected: " << result.actionsRejected << "\n";
                std::cout << "  Assertions failed: " << result.assertionsFailed << "\n";
                if (!result.failures.isEmpty()) {
                    std::cout << "  Failures:\n";
                    for (const QString &failure : result.failures) {
                        std::cout << "    - " << failure.toStdString() << "\n";
                    }
                }
                if (!result.artifacts.isEmpty()) {
                    std::cout << "  Artifacts:\n";
                    for (const QString &artifact : result.artifacts) {
                        std::cout << "    - " << artifact.toStdString() << "\n";
                    }
                }
            }
            std::cout.flush();
            std::cerr.flush();

            // Exit immediately in test mode
            if (testMode) {
                std::exit(result.passed ? 0 : 1);
            }
        } else {
            std::cerr << "Failed to load script: " << script.error().toStdString() << "\n";
            std::cerr.flush();
            if (testMode) {
                std::exit(1);
            }
        }
    } else {
        // Interactive startup. Order matters and must not be changed casually:
        //
        //   1. openMostRecent()      — creates the project and with it the
        //                              central widget (the mixer view).
        //   2. restoreWindowLayout() — QMainWindow::restoreGeometry/restoreState.
        //                              Restoring earlier (without the central
        //                              widget, or before the final show) lands in
        //                              a Qt edge case: a geometry saved maximized
        //                              recreates the window directly maximized,
        //                              no resize transition is delivered, and the
        //                              restored dock/toolbar layout stays frozen
        //                              at the tiny pre-show size — all widgets
        //                              crammed overlapping into the top-left
        //                              corner.
        //   3. show()                — honors the maximized flag restoreGeometry
        //                              put on the window.
        //
        // First run (nothing saved yet): default placement, maximized.
        win->openMostRecent();
        if( win->restoreWindowLayout() ) {
            win->show();
        } else {
            win->move( 100, 100 );
            win->resize( 800, 600 );
            win->showMaximized();
        }
    }

    app.exec();

    // Flush and join the log's file writer before the process tears down.
    TW_LOGI( "ui.shell", "Smaragd exiting" );
    tw::TwLog::instance().shutdown();
    return 0;
}
