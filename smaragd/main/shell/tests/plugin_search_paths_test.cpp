// The stale-INI plugin search-path merge (fix/mac-vst3-loading).
//
// `plugins/searchPaths` is written into a user's INI once and then wins
// forever: SOpt::def() is consulted only when the key is ABSENT, so that an
// emptied list can mean "search nowhere". The cost is that a stored list is
// frozen at the FORMATS that existed the day it was written, and a user whose
// INI predates VST3 hosting (proposal 08 M6) silently gets no VST3 plugins at
// all — every slot resolves to the transparent placeholder with no diagnostic.
//
// This gates the merge that fixes it. It runs on the pure function rather than
// through QSettings, and it STATES its default lists rather than asking the
// box it happens to run on: the macOS shape is the one the field report came
// from, and it has to be assertable from Windows and Linux too.

#include "app/shell/spluginsearchpathmerge.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <iostream>

static int gFailures = 0;

static bool check( bool ok, const QString &what )
{
    std::cout << ( ok ? "  ok   " : "  FAIL " ) << what.toStdString() << std::endl;
    if( !ok ) ++gFailures;
    return ok;
}

static const QStringList kMacDefaults = {
    "/Library/Audio/Plug-Ins/CLAP",
    "/Users/x/Library/Audio/Plug-Ins/CLAP",
    "/Library/Audio/Plug-Ins/VST3",
    "/Users/x/Library/Audio/Plug-Ins/VST3",
};

int main( int argc, char **argv )
{
    QCoreApplication app( argc, argv );
    using namespace spluginpaths;

    // ---- 1: THE FIELD CASE -------------------------------------------------
    //
    // The exact list found in the reporting user's smaragd.ini, byte for byte.
    // CLAP only, because the INI was written before VST3 hosting existed.
    std::cout << "=== the stale macOS INI ===" << std::endl;
    {
        const QStringList stored = { "/Library/Audio/Plug-Ins/CLAP",
                                     "/Users/x/Library/Audio/Plug-Ins/CLAP" };
        const QStringList got = mergeMissingFormatDefaults( stored, kMacDefaults );

        check( got.contains( "/Library/Audio/Plug-Ins/VST3" ) &&
                   got.contains( "/Users/x/Library/Audio/Plug-Ins/VST3" ),
               "both VST3 defaults are merged in" );
        check( got.contains( "/Library/Audio/Plug-Ins/CLAP" ) &&
                   got.contains( "/Users/x/Library/Audio/Plug-Ins/CLAP" ),
               "...and both stored CLAP entries survive" );
        check( got.size() == 4, "...with nothing else added" );
        check( got.at( 0 ) == stored.at( 0 ) && got.at( 1 ) == stored.at( 1 ),
               "the user's own entries keep their order and stay first" );
    }

    // ---- 2: A DELETION STANDS ----------------------------------------------
    //
    // The rule is PER FORMAT, and this is the assertion that pins it there. A
    // user who removed ONE of a format's two standard folders still mentions
    // that format, so nothing of that format comes back. A per-DIRECTORY rule
    // would silently undo every such deletion on the next launch.
    std::cout << "=== a deleted directory is not resurrected ===" << std::endl;
    {
        const QStringList stored = { "/Library/Audio/Plug-Ins/CLAP",
                                     "/Users/x/Library/Audio/Plug-Ins/VST3" };
        const QStringList got = mergeMissingFormatDefaults( stored, kMacDefaults );

        check( !got.contains( "/Users/x/Library/Audio/Plug-Ins/CLAP" ),
               "the deleted per-user CLAP folder stays deleted" );
        check( !got.contains( "/Library/Audio/Plug-Ins/VST3" ),
               "...and the deleted system VST3 folder stays deleted" );
        check( got == stored, "the list is returned exactly as stored" );
    }

    // ---- 3: "SEARCH NOWHERE" IS A REAL ANSWER ------------------------------
    //
    // An EMPTY (but present) list is the one thing the absent/empty
    // distinction exists to express. Merging into it would make the options
    // page unable to turn scanning off.
    std::cout << "=== an emptied list stays empty ===" << std::endl;
    {
        check( mergeMissingFormatDefaults( QStringList(), kMacDefaults ).isEmpty(),
               "an empty stored list is returned empty" );
    }

    // ---- 4: NOTHING TO DO ---------------------------------------------------
    std::cout << "=== a complete list is unchanged ===" << std::endl;
    {
        check( mergeMissingFormatDefaults( kMacDefaults, kMacDefaults ) == kMacDefaults,
               "a list already mentioning every format is returned verbatim" );
    }

    // ---- 5: SPELLING -------------------------------------------------------
    //
    // The folder test has to survive the spellings a real INI carries: a
    // trailing separator (Qt and hand editing both produce them), the Windows
    // backslash, and case, since the folder is upper-case in every installer's
    // layout and users type it either way.
    std::cout << "=== path spellings ===" << std::endl;
    {
        check( formatFolderOf( "/Library/Audio/Plug-Ins/VST3/" ) == "vst3",
               "a trailing slash does not change the folder" );
        check( formatFolderOf( "C:\\Program Files\\Common Files\\VST3" ) == "vst3",
               "a Windows path splits on the backslash" );
        check( formatFolderOf( "/opt/plug-ins/vst3" ) == "vst3",
               "a lower-case folder matches an upper-case default" );

        const QStringList winDefaults = { "C:/Program Files/Common Files/CLAP",
                                          "C:/Program Files/Common Files/VST3" };
        const QStringList got = mergeMissingFormatDefaults(
            { "C:\\Program Files\\Common Files\\CLAP\\" }, winDefaults );
        check( got.contains( "C:/Program Files/Common Files/VST3" ),
               "a Windows INI with a trailing backslash still merges VST3 in" );
        check( got.size() == 2, "...and does not re-add the CLAP folder it already had" );
    }

    // ---- 6: A CUSTOM DIRECTORY ---------------------------------------------
    //
    // A user's own folder named for its format counts as mentioning it — the
    // convention every installer follows. One named anything else does not, so
    // the standard folders are added beside it; that is deliberate and cheap
    // (a directory that does not exist is skipped by the enumerator).
    std::cout << "=== custom directories ===" << std::endl;
    {
        const QStringList got = mergeMissingFormatDefaults(
            { "/Users/x/mysynths/CLAP", "/Users/x/mysynths/VST3" }, kMacDefaults );
        check( got.size() == 2,
               "custom folders named for their formats suppress both defaults" );

        const QStringList got2 = mergeMissingFormatDefaults(
            { "/Users/x/mysynths" }, kMacDefaults );
        check( got2.size() == 5 && got2.at( 0 ) == "/Users/x/mysynths",
               "an unnamed custom folder keeps its place and gains the defaults" );
    }

    std::cout << ( gFailures == 0 ? "PASS" : "FAIL" ) << " - plugin_search_paths_test, "
              << gFailures << " failure(s)" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
