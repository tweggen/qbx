// THE CAPTURE-NAME TEST, which is what stops a user's own file being offered
// for the bin (plan 53 §6.3, P6 / QBX-184).
//
// WHY A UNIT TEST AND NOT A .qxa. `srecfiles::isCaptureName` is only reached by
// the DISK half of `scanUnused`, and that half needs a project DIRECTORY with
// captures in it. A scripted run has neither: the project is untitled, so it
// has no directory, and the recorder writes into the test output dir instead --
// so `unused_recordings_list.qxa` exercises the model half and this predicate
// is unreachable from it. Measured: with the regex replaced by the substring
// "track", every .qxa in the suite still passed.
//
// It is also exactly the shape a unit test is for: a pure function over a
// string, with no project, no Qt platform and no engine behind it.
//
// AND IT IS THE ONE MISTAKE THIS FEATURE MAY NOT MAKE. A loose test offers to
// move a file the user named themselves to the trash. So the cases below are
// mostly NEGATIVE: the names that must not match matter more than the ones
// that must.

#include "app/model/srecordingfiles.h"

#include <QString>
#include <cstdio>

static int gFailures = 0;

static void check( bool cond, const char *what )
{
    if( cond ) {
        std::printf( "PASS %s\n", what );
    } else {
        std::printf( "FAIL %s\n", what );
        ++gFailures;
    }
}

static void yes( const char *name )
{
    check( srecfiles::isCaptureName( QString::fromLatin1( name ) ),
           ( QString::fromLatin1( "is a capture: " ) + name )
               .toLocal8Bit().constData() );
}

static void no( const char *name )
{
    check( !srecfiles::isCaptureName( QString::fromLatin1( name ) ),
           ( QString::fromLatin1( "is NOT a capture: " ) + name )
               .toLocal8Bit().constData() );
}

int main()
{
    std::printf( "=== srecfiles::isCaptureName ===\n" );

    // What the recorder actually writes. The stamp is `yyyyMMdd_HHmmss_zzz`
    // and the index is the armed track's position in the pass.
    yes( "20261014_190949_902_track0.wav" );
    yes( "20261014_190949_902_track7.wav" );
    yes( "20261014_190949_902_track12.wav" );
    yes( "19991231_235959_000_track0.wav" );

    // ...AND THE COMPOSER AGREES WITH THE RECOGNISER. One definition, which is
    // the whole reason this lives in `model` rather than inside the recorder:
    // a scan with its own copy of the format string would drift silently.
    check( srecfiles::isCaptureName(
               srecfiles::captureName( QStringLiteral( "20261014_190949_902" ),
                                       3 ) ),
           "captureName() produces something isCaptureName() accepts" );
    check( srecfiles::captureName( QStringLiteral( "20261014_190949_902" ), 3 )
               == QStringLiteral( "20261014_190949_902_track3.wav" ),
           "captureName() is <stamp>_track<n>.wav" );

    // ---- THE NEGATIVES, which are the point --------------------------------

    // A file the user named. The commonest thing in a project folder.
    no( "guitar.wav" );
    no( "Lead vocal take 3.wav" );
    no( "mix.wav" );

    // Something that merely CONTAINS the word. A substring test would take all
    // of these, and the loose-regex sabotage that no .qxa caught was exactly
    // this shape.
    no( "backing_track.wav" );
    no( "track0.wav" );
    no( "my_track0.wav" );
    no( "drum track.wav" );

    // Anchoring, both ends. A suffix or a prefix must not sneak past.
    no( "20261014_190949_902_track0.wav.bak" );
    no( "copy of 20261014_190949_902_track0.wav" );
    no( "20261014_190949_902_track0.wav " );

    // The stamp's SHAPE. Every field's width is fixed, so a plausible-looking
    // but wrong stamp is not a capture -- it is somebody else's convention.
    no( "2026114_190949_902_track0.wav" );     // 7-digit date
    no( "20261014_19094_902_track0.wav" );     // 5-digit time
    no( "20261014_190949_90_track0.wav" );     // 2-digit millis
    no( "20261014_190949_902_track.wav" );     // no index
    no( "20261014-190949-902_track0.wav" );    // wrong separators

    // Not a WAV. The recorder writes wav and only wav; anything else in the
    // folder is not its business.
    no( "20261014_190949_902_track0.flac" );
    no( "20261014_190949_902_track0" );

    // Degenerate input must simply answer no.
    no( "" );
    no( ".wav" );

    std::printf( "%s (%d failure(s))\n", gFailures == 0 ? "OK" : "FAILED",
                 gFailures );
    return gFailures == 0 ? 0 : 1;
}
