
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

int main( int argc, char *argv[] )
{
    SApplication app( argc, argv );

    // Command-line parsing (Phase 1: --run-actions, Phase 2+: --test-case, Phase 0+: --list-actions)
    QCommandLineParser parser;
    parser.addOption({"run-actions", "Execute an action script and keep the window open.", "file"});
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

    SMainWindow *win = new SMainWindow();
    // Restored (un-maximized) geometry, then start maximized to fill the screen.
    win->move( 100,100 );
    win->resize( 800, 600 );
    win->showMaximized();

    // Execute action script if --run-actions was provided
    if (parser.isSet("run-actions")) {
        QString scriptPath = parser.value("run-actions");
        SActionScript script;
        if (script.readFile(scriptPath)) {
            SActionRunner runner;
            SActionRunner::Result result = runner.run(script, app);
            std::cout << "Script executed: " << (result.passed ? "PASS" : "FAIL") << "\n";
            std::cout << "  Actions applied: " << result.actionsApplied << "\n";
            std::cout << "  Actions rejected: " << result.actionsRejected << "\n";
            if (!result.failures.isEmpty()) {
                std::cout << "  Failures:\n";
                for (const QString &failure : result.failures) {
                    std::cout << "    - " << failure.toStdString() << "\n";
                }
            }
            std::cout.flush();
            std::cerr.flush();
        } else {
            std::cerr << "Failed to load script: " << script.error().toStdString() << "\n";
            std::cerr.flush();
        }
    } else {
        // Re-open the most recently used project (if any still exist on disk).
        win->openMostRecent();
    }

    app.exec();
    return 0;
}
