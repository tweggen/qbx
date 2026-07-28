// Gate for the three storage rules in app/model/sfilepathref.h: an external
// file reference is written PROJECT-RELATIVE, else HOME-RELATIVE ("~/..."),
// else — only when the climb leaves home behind — ABSOLUTE.
//
// Every case is built from QDir::homePath() and QDir::rootPath() so the same
// expectations hold on Windows, Linux and macOS; nothing here touches the disk
// (the encoding is pure path arithmetic, and must stay that way — a rule that
// depended on a file existing would encode differently on the machine that
// saved the project than on the one that opens it).

#include "app/model/sfilepathref.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <iostream>

namespace {

QStringList g_failures;

void check( const QString &what, const QString &got, const QString &expected )
{
    if( got == expected ) {
        std::cout << "PASS: " << what.toStdString() << "  -> "
                  << got.toStdString() << "\n";
        return;
    }
    std::cout << "FAIL: " << what.toStdString() << "\n";
    g_failures.append( QString( "%1\n  expected: %2\n  got:      %3" )
                           .arg( what, expected, got ) );
}

// toStored + fromStored must compose back to the file we started from, for
// every rule. This is the property that actually matters: the spelling is a
// means, reopening the project is the end.
void checkRoundTrip( const QString &what, const QString &file,
                     const QString &project )
{
    const QString stored = SFilePathRef::toStored( file, project );
    const QString back   = SFilePathRef::fromStored( stored, project );
    const QString want   = QDir::cleanPath( QFileInfo( file ).absoluteFilePath() );
    if( back == want ) return;
    std::cout << "FAIL: round trip " << what.toStdString() << "\n";
    g_failures.append( QString( "round trip %1 (stored as '%2')\n"
                                "  expected: %3\n  got:      %4" )
                           .arg( what, stored, want, back ) );
}

} // namespace

int main()
{
    const QString home = QDir::cleanPath( QDir::homePath() );
    const QString root = QDir::cleanPath( QDir::rootPath() );

    // --- Rule 1: relative to the project file ------------------------------
    {
        const QString project = home + "/proj/song.qxp";
        check( "file inside the project folder",
               SFilePathRef::toStored( home + "/proj/samples/a.wav", project ),
               "samples/a.wav" );
        checkRoundTrip( "inside", home + "/proj/samples/a.wav", project );
    }
    {
        // Climbs, but only within a subtree that still moves as one unit.
        const QString project = home + "/proj/sub/song.qxp";
        check( "sibling folder of the project folder",
               SFilePathRef::toStored( home + "/proj/samples/a.wav", project ),
               "../samples/a.wav" );
        checkRoundTrip( "sibling", home + "/proj/samples/a.wav", project );
    }
    {
        // The project sits DIRECTLY in home, so reaching another folder in home
        // climbs nowhere. Rule 1 wins over rule 2 — "~" is for climbs.
        const QString project = home + "/song.qxp";
        check( "project directly in home, file beside it",
               SFilePathRef::toStored( home + "/samples/a.wav", project ),
               "samples/a.wav" );
        checkRoundTrip( "project in home", home + "/samples/a.wav", project );
    }

    // --- Rule 2: relative to home ------------------------------------------
    {
        const QString project = home + "/proj/song.qxp";
        check( "climbs exactly to home",
               SFilePathRef::toStored( home + "/audio/library/a.wav", project ),
               "~/audio/library/a.wav" );
        checkRoundTrip( "to home", home + "/audio/library/a.wav", project );
    }
    {
        const QString project = home + "/a/b/c/song.qxp";
        check( "climbs several levels, still lands on home",
               SFilePathRef::toStored( home + "/x/a.wav", project ),
               "~/x/a.wav" );
        checkRoundTrip( "deep to home", home + "/x/a.wav", project );
    }
    check( "'~/...' expands on read",
           SFilePathRef::fromStored( "~/x/a.wav", home + "/proj/song.qxp" ),
           home + "/x/a.wav" );

    // --- Rule 3: absolute ---------------------------------------------------
    {
        const QString project = home + "/proj/song.qxp";
        const QString outside = root + "smaragd_outside_home.wav";
        check( "climbs past home to the filesystem root",
               SFilePathRef::toStored( outside, project ),
               QDir::cleanPath( outside ) );
        checkRoundTrip( "outside home", outside, project );
    }
    {
        // An untitled project has no anchor to be relative to.
        const QString file = home + "/proj/samples/a.wav";
        check( "untitled project stores absolute",
               SFilePathRef::toStored( file, QString() ),
               QDir::cleanPath( file ) );
    }
#if defined( Q_OS_WIN )
    {
        // No shared root at all. (Pure string work — the drive need not exist.)
        const QString project = home + "/proj/song.qxp";
        check( "different volume stores absolute",
               SFilePathRef::toStored( "Z:/audio/a.wav", project ),
               "Z:/audio/a.wav" );
    }
#endif

    // --- Reading back what other spellings mean -----------------------------
    check( "absolute survives read unchanged",
           SFilePathRef::fromStored( home + "/x/a.wav", home + "/proj/song.qxp" ),
           home + "/x/a.wav" );
    check( "relative resolves against the project folder",
           SFilePathRef::fromStored( "../samples/a.wav", home + "/proj/sub/song.qxp" ),
           home + "/proj/samples/a.wav" );
    check( "relative without an anchor is handed back untouched",
           SFilePathRef::fromStored( "samples/a.wav", QString() ),
           "samples/a.wav" );

    std::cout << "\n";
    if( g_failures.isEmpty() ) {
        std::cout << "All file-path reference cases passed.\n";
        return 0;
    }
    std::cout << g_failures.size() << " failures:\n\n";
    for( const QString &f : g_failures ) std::cout << f.toStdString() << "\n\n";
    return 1;
}
