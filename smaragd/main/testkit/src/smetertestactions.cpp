#include "app/testkit/smetertestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"

#include "tw/metering/tw_level_probe.h"
#include "tw/pages/tw_output_page.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QDomElement>
#include <cmath>

namespace {

// The head lives under the main window, and testkit may not include
// app/timeline — so reach it through the shell, the same route drag-clip-edge
// and assert-lane-alignment use.
SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

// Path-addressed, so a track NESTED inside a folder can be metered. The old
// version scanned the mixer's DIRECT children, which meant a nested lane's
// meter had no automated coverage at all — uncomfortable, because the meters
// consume the mute/solo audibility rule, which is precisely what nesting
// changes.
STrack *laneAtPath( SProject *project, const QString &path )
{
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( path ) );
    return dynamic_cast<STrack *>( lane );
}

}  // namespace

QString SAssertMeterAction::effectivePath() const
{
    return trackPath_.isEmpty() ? QString::number( trackIndex_ ) : trackPath_;
}

SApplyResult SAssertMeterAction::apply( SProject *project )
{
    const QString lanePath = effectivePath();
    STrack *track = laneAtPath( project, lanePath );
    if( !track ) {
        qWarning() << "assert-meter: no track" << lanePath;
        return { false, nullptr };
    }

    std::shared_ptr<twComponent> tap = track->getRootComponent();
    if( !tap ) {
        qWarning() << "assert-meter: track" << lanePath << "has no root component";
        return { false, nullptr };
    }

    const offset_t pos = (offset_t) position_;

    // The probe measures the window ENDING at pos, MIN_WINDOW long after a reset,
    // so the page we must materialise is the one holding the window's START.
    const offset_t CAP = (offset_t) twOutputPage::FRAME_CAPACITY;
    offset_t winStart = pos - twLevelProbe::MIN_WINDOW;
    if( winStart < 0 ) winStart = 0;
    const offset_t pageStart = twFloorAlign( winStart, CAP );

    twLevelProbe probe;
    probe.setTap( tap );

    if( !expectMiss_ ) {
        // Freeze it ourselves rather than waiting for a transport. Legal off the
        // RT thread (the guard in twComponent::freezePage only fires there),
        // deduped against any concurrent revalidation (proposal 19 Phase 2a), and
        // exactly the call the offline render makes.
        // Freeze it ourselves rather than waiting for a transport.
        //
        // CAVEAT this verb deliberately does not paper over: this is the LEGACY
        // PULL path, and a track's twPluginChain sits between the trackmix and
        // the rewire. twStreamingLatch::copyData gates its cached page on the
        // CHAIN's content epoch, which STrack::invalidateRenderPath() does not
        // reach (an SPluginChain is not an SLink child of its track — the same
        // pitfall plugins/CONTRACT.md records for slots). So a gain change made
        // AFTER this position was first frozen is not picked up here, even though
        // playback and render both see it — they go through the scheduler, which
        // re-plans and re-binds instead of consulting that cache.
        // Consequence for tests: measure each gain setting at a position whose
        // page has not been frozen yet (see meter_postfader.qxa, which uses two
        // tracks rather than changing one track's gain twice).
        tap->requestPage( pageStart, nullptr, 0, CAP, project->getSRate(), nullptr );
    }

    twLevelSampleSet set;
    const bool got = probe.advanceTo( pos, set, lanes_ );

    if( expectMiss_ ) {
        if( got ) {
            qWarning() << "assert-meter FAILED: expected a miss at" << pos
                       << "but measured peak" << set.first().peak;
            return { false, nullptr };
        }
        return { true, nullptr };
    }

    if( !got ) {
        qWarning() << "assert-meter FAILED: no frozen page for position" << pos
                   << "(page" << pageStart << ")";
        return { false, nullptr };
    }

    // THE WIDTH, asserted before any level: a page that quietly stayed mono
    // passes every bound on lane 0, which is precisely how a channel gate goes
    // blind (proposal 36 trap 22).
    if( expectLanes_ >= 0 && set.lanes != expectLanes_ ) {
        qWarning() << "assert-meter FAILED: expected" << expectLanes_
                   << "lanes at" << pos << "but the probe reported" << set.lanes;
        return { false, nullptr };
    }

    if( lane_ < 0 || lane_ >= set.lanes ) {
        // An out-of-range lane is an ERROR, never an empty selection reporting
        // rms 0 — the same rule M0 had to fix in the file-reading verbs, where a
        // typo could masquerade as a silent render.
        qWarning() << "assert-meter FAILED: lane" << lane_
                   << "out of range; the probe reported" << set.lanes << "lane(s)";
        return { false, nullptr };
    }

    // Lane-vs-lane: the only assertion that can tell a genuinely wide meter from
    // a duplicated-mono one.
    if( laneA_ >= 0 && laneB_ >= 0 ) {
        if( laneA_ >= set.lanes || laneB_ >= set.lanes ) {
            qWarning() << "assert-meter FAILED: lanes" << laneA_ << "/" << laneB_
                       << "out of range;" << set.lanes << "lane(s) measured";
            return { false, nullptr };
        }
        const double ra = std::sqrt( (double) set.lane[laneA_].meanSquare );
        const double rb = std::sqrt( (double) set.lane[laneB_].meanSquare );
        const double delta = std::fabs( ra - rb );
        if( minLaneRmsDelta_ >= 0.0 && delta < minLaneRmsDelta_ ) {
            qWarning() << "assert-meter FAILED: lanes" << laneA_ << "and" << laneB_
                       << "rms" << ra << "vs" << rb << "differ by" << delta
                       << "which is below" << minLaneRmsDelta_ << "at" << pos;
            return { false, nullptr };
        }
    }

    const twLevelSample &s = set.lane[lane_];
    const double rms = std::sqrt( (double) s.meanSquare );

    if( minRms_ >= 0.0 && rms < minRms_ ) {
        qWarning() << "assert-meter FAILED: rms" << rms << "<" << minRms_
                   << "at" << pos;
        return { false, nullptr };
    }
    if( maxRms_ >= 0.0 && rms > maxRms_ ) {
        qWarning() << "assert-meter FAILED: rms" << rms << ">" << maxRms_
                   << "at" << pos;
        return { false, nullptr };
    }
    if( minPeak_ >= 0.0 && s.peak < minPeak_ ) {
        qWarning() << "assert-meter FAILED: peak" << s.peak << "<" << minPeak_
                   << "at" << pos;
        return { false, nullptr };
    }
    if( maxPeak_ >= 0.0 && s.peak > maxPeak_ ) {
        qWarning() << "assert-meter FAILED: peak" << s.peak << ">" << maxPeak_
                   << "at" << pos;
        return { false, nullptr };
    }

    // Paint the measured level into a PNG: the only thing that exercises
    // SLevelMeter::paintEvent, and an artifact a human can actually look at.
    if( !grabPng_.isEmpty() ) {
        SMainWindow *win = mainWindow();
        if( !win ) {
            qWarning() << "assert-meter FAILED: no main window for grabPng";
            return { false, nullptr };
        }
        if( grabPng_.contains( '/' ) || grabPng_.contains( '\\' ) ||
            grabPng_.contains( ".." ) ) {
            qWarning() << "assert-meter: grabPng contains path separators:" << grabPng_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-meter FAILED: no usable test output directory";
            return { false, nullptr };
        }
        const QString out = QDir( app.testOutputDir() ).filePath( grabPng_ );
        // Wide enough for the lanes the probe actually measured, so the grab of
        // a stereo project shows TWO bars at different heights (the picture that
        // makes AC B8.2 checkable by eye as well as by number).
        const int longSide = 120, shortSide = 10 + 4 * ( set.lanes - 1 );
        if( !win->grabLevelMeter( out, set, grabVertical_,
                                  grabVertical_ ? shortSide : longSide,
                                  grabVertical_ ? longSide : shortSide ) ) {
            qWarning() << "assert-meter FAILED: could not paint the meter into"
                       << out;
            return { false, nullptr };
        }
    }

    // AC B8.4 — the REAL track head, at a named lane height and column width,
    // painted into a PNG. describe() below says what the density rules decided;
    // this says what it LOOKS like, which is the half a string cannot carry.
    if( !grabHead_.isEmpty() ) {
        SMainWindow *win = mainWindow();
        if( !win ) {
            qWarning() << "assert-meter FAILED: no main window for grabHead";
            return { false, nullptr };
        }
        if( grabHead_.contains( '/' ) || grabHead_.contains( '\\' ) ||
            grabHead_.contains( ".." ) ) {
            qWarning() << "assert-meter: grabHead contains path separators:"
                       << grabHead_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-meter FAILED: no usable test output directory";
            return { false, nullptr };
        }
        const QString out = QDir( app.testOutputDir() ).filePath( grabHead_ );
        if( !win->grabTrackHead( lanePath, out, headHeight_, headWidth_, set ) ) {
            qWarning() << "assert-meter FAILED: could not paint the track head"
                       << lanePath << "into" << out;
            return { false, nullptr };
        }
    }

    // Widget half: build the real head and check what its meter reports. This is
    // the only automated coverage the density rules have.
    if( headHeight_ > 0 && !contains_.isEmpty() ) {
        SMainWindow *win = mainWindow();
        if( !win ) {
            qWarning() << "assert-meter FAILED: no main window for headHeight";
            return { false, nullptr };
        }
        const QString desc =
            win->describeTrackMeter( lanePath, headHeight_, headWidth_ );
        if( desc.isEmpty() ) {
            qWarning() << "assert-meter FAILED: no meter description for track"
                       << lanePath;
            return { false, nullptr };
        }
        if( !desc.contains( contains_ ) ) {
            qWarning() << "assert-meter FAILED: head at height" << headHeight_
                       << "width" << headWidth_
                       << "reports" << desc << "which lacks" << contains_;
            return { false, nullptr };
        }
    }

    return { true, nullptr };   // an assertion has nothing to undo
}

void SAssertMeterAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "trackIndex", trackIndex_ );
    elem.setAttribute( "position", QString::number( position_ ) );
    elem.setAttribute( "minRms", QString::number( minRms_ ) );
    elem.setAttribute( "maxRms", QString::number( maxRms_ ) );
    elem.setAttribute( "minPeak", QString::number( minPeak_ ) );
    elem.setAttribute( "maxPeak", QString::number( maxPeak_ ) );
    elem.setAttribute( "expectMiss", expectMiss_ ? "true" : "false" );
    elem.setAttribute( "headHeight", headHeight_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "lanes", lanes_ );
    elem.setAttribute( "expectLanes", expectLanes_ );
    elem.setAttribute( "lane", lane_ );
    elem.setAttribute( "laneA", laneA_ );
    elem.setAttribute( "laneB", laneB_ );
    elem.setAttribute( "minLaneRmsDelta", QString::number( minLaneRmsDelta_ ) );
    elem.setAttribute( "headWidth", headWidth_ );
}

bool SAssertMeterAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_  = elem.attribute( "trackPath" );
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    position_   = elem.attribute( "position", "0" ).toLongLong();
    minRms_     = elem.attribute( "minRms", "-1" ).toDouble();
    maxRms_     = elem.attribute( "maxRms", "-1" ).toDouble();
    minPeak_    = elem.attribute( "minPeak", "-1" ).toDouble();
    maxPeak_    = elem.attribute( "maxPeak", "-1" ).toDouble();
    expectMiss_ = elem.attribute( "expectMiss", "false" ) == "true";
    headHeight_ = elem.attribute( "headHeight", "0" ).toInt();
    contains_   = elem.attribute( "contains" );
    grabPng_    = elem.attribute( "grabPng" );
    grabVertical_ = elem.attribute( "grabVertical", "true" ) != "false";
    lanes_       = elem.attribute( "lanes", "1" ).toInt();
    expectLanes_ = elem.attribute( "expectLanes", "-1" ).toInt();
    lane_        = elem.attribute( "lane", "0" ).toInt();
    laneA_       = elem.attribute( "laneA", "-1" ).toInt();
    laneB_       = elem.attribute( "laneB", "-1" ).toInt();
    minLaneRmsDelta_ = elem.attribute( "minLaneRmsDelta", "-1" ).toDouble();
    grabHead_    = elem.attribute( "grabHead" );
    headWidth_   = elem.attribute( "headWidth", "0" ).toInt();
    // Asking for a lane comparison implies asking for at least that many lanes:
    // a case that says laneA/laneB but forgets `lanes` would otherwise get one
    // lane and fail with an out-of-range message instead of a comparison.
    const int need = qMax( qMax( lane_, qMax( laneA_, laneB_ ) ) + 1, expectLanes_ );
    if( need > lanes_ ) lanes_ = need;
    return true;
}

static const bool s_reg_assertmeter =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-meter" ),
          [] { return new SAssertMeterAction; } ),
      true );
