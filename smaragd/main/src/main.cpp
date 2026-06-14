
#include <qapplication.h>
#include <QFontDatabase>
#include <QFont>
#include <QCommandLineParser>
#include "sapplication.h"
#include "smainwindow.h"
#include "sactionscript.h"
#include "sactionrunner.h"
#include "sactionregistry.h"
#include <iostream>
#include <cstdlib>
#include <cstring>

int main( int argc, char *argv[] )
{
    // Phase 4: Detect headless test mode before QApplication init to set platform
    bool headlessMode = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--test-case") == 0) {
            headlessMode = true;
            break;
        }
    }

    // If in headless test mode and -platform not explicitly set, use offscreen
    if (headlessMode) {
        bool platformSet = false;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-platform") == 0) {
                platformSet = true;
                break;
            }
        }
        if (!platformSet) {
            // Insert -platform offscreen before creating QApplication
            int newArgc = argc + 2;
            char **newArgv = new char*[newArgc];
            newArgv[0] = argv[0];  // program name
            newArgv[1] = (char*)"-platform";
            newArgv[2] = (char*)"offscreen";
            for (int i = 1; i < argc; ++i) {
                newArgv[i + 2] = argv[i];
            }
            argc = newArgc;
            argv = newArgv;
        }
    }

    SApplication app( argc, argv );

    // Command-line parsing (Phase 1+: --run-actions, Phase 2+: --test-case, --list-actions)
    QCommandLineParser parser;
    parser.addOption({"run-actions", "Execute an action script and keep the window open.", "file"});
    parser.addOption({"test-case", "Run an action script as a headless test (exit 0 on pass, 1 on fail).", "file"});
    parser.addOption({"list-actions", "List known action verbs and exit."});
    parser.process(app);

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

    // Determine test mode vs interactive mode
    bool testMode = parser.isSet("test-case");
    bool runMode = parser.isSet("run-actions");
    QString scriptPath = testMode ? parser.value("test-case") : parser.value("run-actions");

    // Create main window (only shown in interactive mode)
    SMainWindow *win = new SMainWindow();
    if (!testMode) {
        // Restored (un-maximized) geometry, then start maximized to fill the screen.
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
        // Re-open the most recently used project (if any still exist on disk).
        win->openMostRecent();
    }

    app.exec();
    return 0;
}
