#pragma once

// Merging a STORED plugin search-path list with the per-platform defaults.
//
// This is a pure function on two string lists, in its own header, for one
// reason: it is the whole of what `SSettings::pluginSearchPaths()` decides,
// and it must be gateable without a QSettings store, a config directory or a
// platform (`plugin_search_paths_test`). The caller supplies the defaults, so
// the test can state the Windows, macOS and Linux shapes explicitly rather
// than asserting whatever the box it happens to run on produces.
//
// THE PROBLEM IT SOLVES. `plugins/searchPaths` is written into the user's INI
// once and then wins forever — `SOpt::def()` is consulted only when the key is
// ABSENT, deliberately, so that a user who empties the list means "search
// nowhere" rather than "give me the defaults back". The cost is that a stored
// list is frozen at the set of FORMATS that existed the day it was written.
// Observed in the field on macOS, where an INI predating VST3 hosting
// (proposal 08 M6) read exactly:
//
//   searchPaths=/Library/Audio/Plug-Ins/CLAP, ~/Library/Audio/Plug-Ins/CLAP
//
// so the scanner never walked a VST3 directory, every VST3 slot in every
// project resolved to the transparent placeholder, and the user was told
// nothing — the plugins were simply, silently absent.

#include <QSet>
#include <QString>
#include <QStringList>

namespace spluginpaths {

// The last path component, lower-cased: "clap" for ".../Audio/Plug-Ins/CLAP".
// Every hosted format's standard locations end in a folder NAMED for the format
// on all three platforms (`twPluginSearchPaths::defaults`), which is what makes
// this a usable "does the stored list know about this format at all?" test
// without teaching SSettings a second copy of the format list.
inline QString formatFolderOf( const QString &dir )
{
    QString d = dir.trimmed();
    while( d.size() > 1 && ( d.endsWith( '/' ) || d.endsWith( '\\' ) ) )
        d.chop( 1 );
    const int slash = qMax( d.lastIndexOf( '/' ), d.lastIndexOf( '\\' ) );
    return ( slash < 0 ? d : d.mid( slash + 1 ) ).toLower();
}

// Append the defaults of every format `stored` does not mention AT ALL.
//
// The test is PER FORMAT and never per directory, and that is the whole design:
// a user who deleted one of a format's two standard folders still MENTIONS that
// format, so their deletion stands. Only a format with no representation
// whatsoever gets its defaults back — the shape a version skew produces, and
// one a user is unlikely to reach by hand, because the options page offers a
// format's folders together.
//
// An EMPTY stored list is returned untouched: "search nowhere" is a real answer
// and merging into it would override the one thing the absent/empty distinction
// exists to express.
//
// A custom directory counts as mentioning its format when it is itself named
// for it (".../mysynths/VST3"), which is the convention every plugin installer
// follows. A custom directory named anything else does not, so the format's
// standard folders are added alongside it — harmless: a directory that does not
// exist is skipped by `enumeratePluginModules` and costs a stat.
inline QStringList mergeMissingFormatDefaults( const QStringList &stored,
                                               const QStringList &defaults )
{
    if( stored.isEmpty() ) return stored;

    QSet<QString> mentioned;
    for( const QString &d : stored )
        mentioned.insert( formatFolderOf( d ) );

    QStringList out = stored;
    for( const QString &d : defaults ) {
        if( mentioned.contains( formatFolderOf( d ) ) ) continue;
        out << d;
    }
    out.removeDuplicates();
    return out;
}

}  // namespace spluginpaths
