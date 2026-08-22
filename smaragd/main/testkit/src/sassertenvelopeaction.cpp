#include "app/testkit/sassertenvelopeaction.h"

#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QDomElement>
#include <QMap>
#include <QVector>
#include <QWidget>

#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/sapplication.h"

namespace {

// The arranger and its objects live under the main window, and testkit may not
// include app/timeline - so reach the collect seam through the shell, the same
// route drag-clip-edge and assert-track-head take (testkit CONTRACT inv. 5).
SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    return nullptr;
}

// The named probe arrays. Process-global on purpose: one .qxa script runs per
// process, so a snapshot taken by one action is visible to every later one in
// the same script and to nothing at all outside it. Keeping it here rather than
// on SActionRunner keeps the runner's Result struct about pass/fail.
QMap<QString, QVector<preview_t> > &snapshots()
{
    static QMap<QString, QVector<preview_t> > s;
    return s;
}

QString describeProbes( const QVector<preview_t> &pv, int from, int count )
{
    QString out;
    for( int i = from; i < from + count && i < pv.size(); ++i ) {
        if( !out.isEmpty() ) out += QStringLiteral( " " );
        out += QStringLiteral( "[%1]%2/%3" )
                   .arg( i ).arg( (int) pv[i].min ).arg( (int) pv[i].max );
    }
    return out;
}

}  // namespace

SApplyResult SAssertEnvelopeAction::apply( SProject * )
{
    const bool childSum = ( mode_ == QStringLiteral( "childSum" ) );
    if( !childSum && mode_ != QStringLiteral( "clip" ) ) {
        // Never silently fall back to the clip walk: a script that asked for a
        // mode this build does not have must FAIL, not measure something else.
        qWarning() << "assert-envelope: unsupported mode" << mode_
                   << "(this build implements mode=clip and mode=childSum)";
        return { false, nullptr };
    }
    if( childSum ? trackPath_.isEmpty() : clipPath_.isEmpty() ) {
        qWarning() << "assert-envelope: mode" << mode_ << "needs"
                   << ( childSum ? "trackPath=" : "clip=" );
        return { false, nullptr };
    }
    if( width_ < 1 ) {
        qWarning() << "assert-envelope: width must be >= 1, got" << width_;
        return { false, nullptr };
    }

    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-envelope: no main window";
        return { false, nullptr };
    }

    // ONE verb, two collect terminals - and both are the call the PAINTER
    // makes, never a re-derivation of it. childSum reads the folder-sum overlay
    // (proposal 39 M3), so a script's number and the lane's pixels come out of
    // the same walk.
    std::vector<preview_t> raw;
    const bool got =
        childSum
            ? win->collectTrackChildSumEnvelope( trackPath_, (offset_t) start_,
                                                 (length_t) length_, width_, raw )
            : win->collectClipEnvelope( clipPath_, (offset_t) start_,
                                        (length_t) length_, width_, raw );

    QVector<preview_t> pv;
    pv.reserve( width_ );
    for( size_t i = 0; i < raw.size(); ++i ) pv.append( raw[i] );

    bool allZero = true;
    for( int i = 0; i < pv.size(); ++i )
        if( pv[i].min != 0 || pv[i].max != 0 ) { allZero = false; break; }

    const QString where =
        QString( "%1 %2 [%3, %4) over %5 columns" )
            .arg( childSum ? "childSum of track" : "clip" )
            .arg( childSum ? trackPath_ : clipPath_ ).arg( (qlonglong) start_ )
            .arg( (qlonglong)( start_ + length_ ) ).arg( width_ );

    // --- expectEmpty: the collect produced nothing at all -------------------
    if( expectEmpty_ ) {
        if( got && !allZero ) {
            qWarning() << "assert-envelope FAILED:" << where
                       << "- expected an EMPTY envelope, got"
                       << describeProbes( pv, 0, 8 );
            return { false, nullptr };
        }
        qDebug() << "assert-envelope:" << where << "is empty, as expected"
                 << ( got ? "(all-zero probes)" : "(the renderer declined)" );
        return { true, nullptr };
    }

    if( !got ) {
        qWarning() << "assert-envelope FAILED:" << where
                   << "- the collect produced no envelope (nothing at that"
                      " path, no inline renderer, not a folder, or nothing"
                      " below it contributed)";
        return { false, nullptr };
    }

    // --- one column's values ------------------------------------------------
    if( column_ >= 0 ) {
        if( column_ >= pv.size() ) {
            qWarning() << "assert-envelope FAILED:" << where
                       << "- column" << column_ << "is outside the width";
            return { false, nullptr };
        }
        const int gmin = (int) pv[column_].min;
        const int gmax = (int) pv[column_].max;
        if( qAbs( gmin - min_ ) > tolerance_ || qAbs( gmax - max_ ) > tolerance_ ) {
            qWarning() << "assert-envelope FAILED:" << where << "- column"
                       << column_ << "is min" << gmin << "max" << gmax
                       << "- expected min" << min_ << "max" << max_
                       << "within" << tolerance_;
            return { false, nullptr };
        }
        qDebug() << "assert-envelope:" << where << "- column" << column_
                 << "min" << gmin << "max" << gmax << "OK";
    }

    // --- BYTE-IDENTICAL against an earlier snapshot -------------------------
    if( !compareTo_.isEmpty() ) {
        if( !snapshots().contains( compareTo_ ) ) {
            qWarning() << "assert-envelope FAILED:" << where
                       << "- no snapshot named" << compareTo_
                       << "(take one with snapshot= first)";
            return { false, nullptr };
        }
        const QVector<preview_t> was = snapshots()[compareTo_];
        if( was.size() != pv.size() ) {
            qWarning() << "assert-envelope FAILED:" << where
                       << "- snapshot" << compareTo_ << "has" << was.size()
                       << "columns, this collect has" << pv.size();
            return { false, nullptr };
        }
        for( int i = 0; i < pv.size(); ++i ) {
            if( was[i].min == pv[i].min && was[i].max == pv[i].max ) continue;
            qWarning() << "assert-envelope FAILED:" << where
                       << "- differs from snapshot" << compareTo_
                       << "at column" << i
                       << ": was min" << (int) was[i].min
                       << "max" << (int) was[i].max
                       << ", now min" << (int) pv[i].min
                       << "max" << (int) pv[i].max;
            return { false, nullptr };
        }
        qDebug() << "assert-envelope:" << where << "is BYTE-IDENTICAL to"
                 << compareTo_ << "over" << pv.size() << "columns";
    }

    if( !snapshot_.isEmpty() ) {
        snapshots()[snapshot_] = pv;
        qDebug() << "assert-envelope: stored" << where << "as" << snapshot_
                 << describeProbes( pv, 0, 4 );
    }

    return { true, nullptr };   // an assertion has nothing to undo
}

void SAssertEnvelopeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clipPath_ );
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "mode", mode_ );
    elem.setAttribute( "start", QString::number( (qlonglong) start_ ) );
    elem.setAttribute( "length", QString::number( (qlonglong) length_ ) );
    elem.setAttribute( "width", width_ );
    elem.setAttribute( "column", column_ );
    elem.setAttribute( "min", min_ );
    elem.setAttribute( "max", max_ );
    elem.setAttribute( "tolerance", tolerance_ );
    elem.setAttribute( "expectEmpty", expectEmpty_ ? "true" : "false" );
    if( !snapshot_.isEmpty() )  elem.setAttribute( "snapshot", snapshot_ );
    if( !compareTo_.isEmpty() ) elem.setAttribute( "compareTo", compareTo_ );
}

bool SAssertEnvelopeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // An empty path is reported by apply(), not here: a readXml that fails
    // makes the action undeserializable and breaks the round-trip audit, which
    // feeds every verb a DEFAULT instance through write->read->write.
    clipPath_    = elem.attribute( "clip" );
    trackPath_   = elem.attribute( "trackPath" );
    mode_        = elem.attribute( "mode", "clip" );
    start_       = elem.attribute( "start", "0" ).toLongLong();
    length_      = elem.attribute( "length", "0" ).toLongLong();
    width_       = elem.attribute( "width", "64" ).toInt();
    column_      = elem.attribute( "column", "-1" ).toInt();
    min_         = elem.attribute( "min", "0" ).toInt();
    max_         = elem.attribute( "max", "0" ).toInt();
    tolerance_   = elem.attribute( "tolerance", "0" ).toInt();
    expectEmpty_ = elem.attribute( "expectEmpty", "false" ) == "true";
    snapshot_    = elem.attribute( "snapshot" );
    compareTo_   = elem.attribute( "compareTo" );
    return true;
}

static const bool s_reg_assert_envelope =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-envelope" ),
          [] { return new SAssertEnvelopeAction; } ),
      true );


// --- assert-lane-overlay (proposal 39 M3.10) ------------------------------

SApplyResult SAssertLaneOverlayAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-lane-overlay: no main window";
        return { false, nullptr };
    }

    QString png;
    if( !grabPng_.isEmpty() ) {
        if( grabPng_.contains( '/' ) || grabPng_.contains( QLatin1Char( 0x5c ) )
            || grabPng_.contains( ".." ) ) {
            qWarning() << "assert-lane-overlay: grabPng contains path"
                          " separators:" << grabPng_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-lane-overlay FAILED: no usable test output"
                          " directory";
            return { false, nullptr };
        }
        png = QDir( app.testOutputDir() ).filePath( grabPng_ );
    }

    const QString rep = win->describeLaneOverlay( trackPath_, grabWidth_,
                                                  grabHeight_, png, bandOnly_ );
    if( rep.isEmpty() ) {
        qWarning() << "assert-lane-overlay FAILED: no lane at" << trackPath_
                   << "(or the canvas could not be grabbed)";
        return { false, nullptr };
    }
    qDebug() << "assert-lane-overlay: track" << trackPath_ << rep;

    // Parse the two counts back out of the report. It is a describe() line for
    // the same reason describeTrackHead is one: the numbers belong in the log
    // of every run, pass or fail, and a struct crossing the shell boundary
    // would put them nowhere.
    auto field = [&rep]( const QString &key ) -> long long {
        const int i = rep.indexOf( key + QLatin1Char( '=' ) );
        if( i < 0 ) return -1;
        int j = i + key.size() + 1;
        int k = j;
        while( k < rep.size() && ( rep[k].isDigit() || rep[k] == '-' ) ) ++k;
        return rep.mid( j, k - j ).toLongLong();
    };
    const long long overlay  = field( QStringLiteral( "overlayPixels" ) );
    const long long fillPx   = field( QStringLiteral( "fillPixels" ) );
    const long long darker   = field( QStringLiteral( "darkerThanFill" ) );
    const long long lighter  = field( QStringLiteral( "lighterThanClip" ) );
    const long long lutPx    = field( QStringLiteral( "lutPixels" ) );
    const long long lutMin   = field( QStringLiteral( "lutIndexMin" ) );
    const long long lutMax   = field( QStringLiteral( "lutIndexMax" ) );

    if( expectOverlay_ ) {
        // The fill must be ON SCREEN, or "lighter than the fill" is a relation
        // against a colour nothing painted.
        if( fillPx <= 0 ) {
            qWarning() << "assert-lane-overlay FAILED:" << trackPath_
                       << "- the lane fill colour is nowhere in the band:" << rep;
            return { false, nullptr };
        }
        if( overlay < minPixels_ ) {
            qWarning() << "assert-lane-overlay FAILED:" << trackPath_
                       << "- expected at least" << minPixels_
                       << "overlay pixels (strictly lighter than the lane fill,"
                          " strictly darker than the clip body), got" << overlay
                       << "- darkerThanFill" << darker
                       << "lighterThanClip" << lighter << "|" << rep;
            return { false, nullptr };
        }
    } else {
        // Default ceiling is exactly 0 -- byte-for-byte the ORIGINAL
        // behaviour of this branch (proposal 39 M3.10) when maxPixels_ is
        // unset. maxPixels_ (proposal 40 M2) lets a case name an intrinsic
        // noise floor explicitly instead of pretending the classifier can
        // reach a literal zero over real waveform content -- see this
        // action's own header doc for the measurement that forced it.
        const long long ceiling = maxPixels_ >= 0 ? maxPixels_ : 0;
        if( overlay > ceiling ) {
            qWarning() << "assert-lane-overlay FAILED:" << trackPath_
                       << "- expected at most" << ceiling
                       << "overlay pixels, found" << overlay
                       << "pixels |" << rep;
            return { false, nullptr };
        }
    }

    // --- the LUT gate (proposal 40 M2 palette follow-up, 2026-08-21) -------
    // Independent of the expectOverlay_ branch above (a different colour law,
    // a different classifier), and STRICTLY additive: unset (-1, the
    // default) runs neither check, so every pre-existing case is untouched.
    if( minLutPixels_ >= 0 && lutPx < minLutPixels_ ) {
        qWarning() << "assert-lane-overlay FAILED:" << trackPath_
                   << "- expected at least" << minLutPixels_
                   << "LUT (exact palette-colour) pixels, got" << lutPx
                   << "|" << rep;
        return { false, nullptr };
    }
    if( maxLutPixels_ >= 0 && lutPx > maxLutPixels_ ) {
        qWarning() << "assert-lane-overlay FAILED:" << trackPath_
                   << "- expected at most" << maxLutPixels_
                   << "LUT (exact palette-colour) pixels, found" << lutPx
                   << "|" << rep;
        return { false, nullptr };
    }
    if( minLutSpread_ >= 0 && ( lutMax - lutMin ) < minLutSpread_ ) {
        qWarning() << "assert-lane-overlay FAILED:" << trackPath_
                   << "- expected a palette index spread of at least"
                   << minLutSpread_ << "(lutIndexMax - lutIndexMin), got"
                   << ( lutMax - lutMin ) << "|" << rep;
        return { false, nullptr };
    }

    if( !contains_.isEmpty() && !rep.contains( contains_ ) ) {
        qWarning() << "assert-lane-overlay FAILED:" << trackPath_
                   << "- report does not contain" << contains_ << "|" << rep;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertLaneOverlayAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "expectOverlay", expectOverlay_ ? "true" : "false" );
    elem.setAttribute( "minPixels", minPixels_ );
    if( grabWidth_ > 0 )       elem.setAttribute( "grabWidth", grabWidth_ );
    if( grabHeight_ > 0 )      elem.setAttribute( "grabHeight", grabHeight_ );
    if( !grabPng_.isEmpty() )  elem.setAttribute( "grabPng", grabPng_ );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    // Written only when true / set, mirroring every other "default off" flag
    // in this file — every case predating proposal 40 M2 round-trips
    // byte-unchanged.
    if( bandOnly_ )            elem.setAttribute( "bandOnly", "true" );
    if( maxPixels_ >= 0 )      elem.setAttribute( "maxPixels", maxPixels_ );
    // Same "written only when set" discipline as maxPixels_ above, for the
    // same reason: every case predating proposal 40's 2026-08-21 palette
    // follow-up round-trips byte-unchanged.
    if( minLutPixels_ >= 0 )   elem.setAttribute( "minLutPixels", minLutPixels_ );
    if( maxLutPixels_ >= 0 )   elem.setAttribute( "maxLutPixels", maxLutPixels_ );
    if( minLutSpread_ >= 0 )   elem.setAttribute( "minLutSpread", minLutSpread_ );
}

bool SAssertLaneOverlayAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_     = elem.attribute( "trackPath", "0" );
    expectOverlay_ = elem.attribute( "expectOverlay", "true" ) == "true";
    minPixels_     = elem.attribute( "minPixels", "1" ).toInt();
    grabWidth_     = elem.attribute( "grabWidth", "0" ).toInt();
    grabHeight_    = elem.attribute( "grabHeight", "0" ).toInt();
    grabPng_       = elem.attribute( "grabPng" );
    contains_      = elem.attribute( "contains" );
    bandOnly_      = elem.attribute( "bandOnly", "false" ) == "true";
    maxPixels_     = elem.attribute( "maxPixels", "-1" ).toInt();
    minLutPixels_  = elem.attribute( "minLutPixels", "-1" ).toInt();
    maxLutPixels_  = elem.attribute( "maxLutPixels", "-1" ).toInt();
    minLutSpread_  = elem.attribute( "minLutSpread", "-1" ).toInt();
    return true;
}

static const bool s_reg_assert_lane_overlay =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-lane-overlay" ),
          [] { return new SAssertLaneOverlayAction; } ),
      true );

// --- assert-take-lane -----------------------------------------------------

SApplyResult SAssertTakeLaneAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-take-lane: no main window";
        return { false, nullptr };
    }

    QString png;
    if( !grabPng_.isEmpty() ) {
        if( grabPng_.contains( '/' ) || grabPng_.contains( QLatin1Char( 0x5c ) )
            || grabPng_.contains( ".." ) ) {
            qWarning() << "assert-take-lane: grabPng contains path"
                          " separators:" << grabPng_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-take-lane FAILED: no usable test output"
                          " directory";
            return { false, nullptr };
        }
        png = QDir( app.testOutputDir() ).filePath( grabPng_ );
    }

    const QString rep = win->describeTakeLane( trackPath_, takeRow_,
                                               grabWidth_, grabHeight_, png );
    if( rep.isEmpty() ) {
        qWarning() << "assert-take-lane FAILED: no take lane at" << trackPath_
                   << "take" << takeRow_ << "(or the canvas could not be"
                      " grabbed)";
        return { false, nullptr };
    }
    qDebug() << "assert-take-lane: track" << trackPath_ << "take" << takeRow_
             << rep;

    auto field = [&rep]( const QString &key ) -> long long {
        const int i = rep.indexOf( key + QLatin1Char( '=' ) );
        if( i < 0 ) return -1;
        int j = i + key.size() + 1;
        int k = j;
        while( k < rep.size() && ( rep[k].isDigit() || rep[k] == '-' ) ) ++k;
        return rep.mid( j, k - j ).toLongLong();
    };
    const long long gapCols   = field( QStringLiteral( "gapCols" ) );
    const long long gapRuns   = field( QStringLiteral( "gapRuns" ) );
    const long long gStartPct = field( QStringLiteral( "firstGapStartPct" ) );
    const long long gEndPct   = field( QStringLiteral( "firstGapEndPct" ) );
    const long long wavePct   = field( QStringLiteral( "waveMeanPct" ) );
    const long long spanFirst = field( QStringLiteral( "spanFirst" ) );
    const long long spanLast  = field( QStringLiteral( "spanLast" ) );

    auto fail = [&]( const QString &what ) {
        qWarning() << "assert-take-lane FAILED:" << trackPath_
                   << "take" << takeRow_ << "-" << what << "|" << rep;
        return SApplyResult{ false, nullptr };
    };

    if( minGapCols_ >= 0 && gapCols < minGapCols_ )
        return fail( QStringLiteral( "expected at least %1 gap columns, got %2"
                                     " - a take lane that fills its whole"
                                     " window paints the WINDOW, not the"
                                     " MATERIAL" )
                         .arg( minGapCols_ ).arg( gapCols ) );
    if( maxGapCols_ >= 0 && gapCols > maxGapCols_ )
        return fail( QStringLiteral( "expected at most %1 gap columns, got %2" )
                         .arg( maxGapCols_ ).arg( gapCols ) );
    if( gapRuns_ >= 0 && gapRuns != gapRuns_ )
        return fail( QStringLiteral( "expected %1 gap runs, got %2" )
                         .arg( gapRuns_ ).arg( gapRuns ) );
    if( firstGapStartPct_ >= 0
        && qAbs( (int) gStartPct - firstGapStartPct_ ) > pctTolerance_ )
        return fail( QStringLiteral( "first gap starts at %1%% of the clip,"
                                     " expected %2%% +/- %3 - the take lane is"
                                     " showing the WRONG PART of the take" )
                         .arg( gStartPct ).arg( firstGapStartPct_ )
                         .arg( pctTolerance_ ) );
    if( firstGapEndPct_ >= 0
        && qAbs( (int) gEndPct - firstGapEndPct_ ) > pctTolerance_ )
        return fail( QStringLiteral( "first gap ends at %1%% of the clip,"
                                     " expected %2%% +/- %3" )
                         .arg( gEndPct ).arg( firstGapEndPct_ )
                         .arg( pctTolerance_ ) );
    if( minWaveMeanPct_ >= 0 && wavePct < minWaveMeanPct_ )
        return fail( QStringLiteral( "mean drawn waveform height %1%%, expected"
                                     " at least %2%%" )
                         .arg( wavePct ).arg( minWaveMeanPct_ ) );
    if( maxWaveMeanPct_ >= 0 && wavePct > maxWaveMeanPct_ )
        return fail( QStringLiteral( "mean drawn waveform height %1%%, expected"
                                     " at most %2%%" )
                         .arg( wavePct ).arg( maxWaveMeanPct_ ) );
    if( spanFirst_ >= 0 && qAbs( (int) spanFirst - spanFirst_ ) > spanTolerance_ )
        return fail( QStringLiteral( "clip span starts at x=%1, expected %2"
                                     " +/- %3" )
                         .arg( spanFirst ).arg( spanFirst_ )
                         .arg( spanTolerance_ ) );
    if( spanLast_ >= 0 && qAbs( (int) spanLast - spanLast_ ) > spanTolerance_ )
        return fail( QStringLiteral( "clip span ends at x=%1, expected %2"
                                     " +/- %3" )
                         .arg( spanLast ).arg( spanLast_ )
                         .arg( spanTolerance_ ) );
    if( !contains_.isEmpty() && !rep.contains( contains_ ) )
        return fail( QStringLiteral( "report does not contain '%1'" )
                         .arg( contains_ ) );

    return { true, nullptr };
}

void SAssertTakeLaneAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "takeRow", takeRow_ );
    if( grabWidth_  > 0 ) elem.setAttribute( "grabWidth", grabWidth_ );
    if( grabHeight_ > 0 ) elem.setAttribute( "grabHeight", grabHeight_ );
    if( !grabPng_.isEmpty() )  elem.setAttribute( "grabPng", grabPng_ );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    if( minGapCols_       >= 0 ) elem.setAttribute( "minGapCols", minGapCols_ );
    if( maxGapCols_       >= 0 ) elem.setAttribute( "maxGapCols", maxGapCols_ );
    if( gapRuns_          >= 0 ) elem.setAttribute( "gapRuns", gapRuns_ );
    if( firstGapStartPct_ >= 0 )
        elem.setAttribute( "firstGapStartPct", firstGapStartPct_ );
    if( firstGapEndPct_   >= 0 )
        elem.setAttribute( "firstGapEndPct", firstGapEndPct_ );
    elem.setAttribute( "pctTolerance", pctTolerance_ );
    if( minWaveMeanPct_   >= 0 )
        elem.setAttribute( "minWaveMeanPct", minWaveMeanPct_ );
    if( maxWaveMeanPct_   >= 0 )
        elem.setAttribute( "maxWaveMeanPct", maxWaveMeanPct_ );
    if( spanFirst_        >= 0 ) elem.setAttribute( "spanFirst", spanFirst_ );
    if( spanLast_         >= 0 ) elem.setAttribute( "spanLast", spanLast_ );
    elem.setAttribute( "spanTolerance", spanTolerance_ );
}

bool SAssertTakeLaneAction::readXml( const QDomElement &elem, int )
{
    trackPath_        = elem.attribute( "trackPath", "0" );
    takeRow_          = elem.attribute( "takeRow", "0" ).toInt();
    grabWidth_        = elem.attribute( "grabWidth", "0" ).toInt();
    grabHeight_       = elem.attribute( "grabHeight", "0" ).toInt();
    grabPng_          = elem.attribute( "grabPng" );
    contains_         = elem.attribute( "contains" );
    minGapCols_       = elem.attribute( "minGapCols", "-1" ).toInt();
    maxGapCols_       = elem.attribute( "maxGapCols", "-1" ).toInt();
    gapRuns_          = elem.attribute( "gapRuns", "-1" ).toInt();
    firstGapStartPct_ = elem.attribute( "firstGapStartPct", "-1" ).toInt();
    firstGapEndPct_   = elem.attribute( "firstGapEndPct", "-1" ).toInt();
    pctTolerance_     = elem.attribute( "pctTolerance", "3" ).toInt();
    minWaveMeanPct_   = elem.attribute( "minWaveMeanPct", "-1" ).toInt();
    maxWaveMeanPct_   = elem.attribute( "maxWaveMeanPct", "-1" ).toInt();
    spanFirst_        = elem.attribute( "spanFirst", "-1" ).toInt();
    spanLast_         = elem.attribute( "spanLast", "-1" ).toInt();
    spanTolerance_    = elem.attribute( "spanTolerance", "2" ).toInt();
    return true;
}

static const bool s_reg_assert_take_lane =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-take-lane" ),
          [] { return new SAssertTakeLaneAction; } ),
      true );
