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
        // This is the LEGACY PULL path, and it used to carry a caveat this verb
        // deliberately did not paper over: twStreamingLatch::copyData gates its
        // cached page on the content epoch of the rewire's PRODUCER, which was
        // the twPluginChain — so a gain change made after this position was
        // first frozen was not picked up here, and a test had to measure each
        // gain setting at a position whose page did not exist yet.
        //
        // RETIRED by proposal 37 P3a: the fader moved out of twTrackMix into
        // twGainStage, which IS the rewire's producer, and set-track-volume
        // bumps exactly the epoch that gate consults. meter_gain_after_probe.qxa
        // is the case that would have been impossible to write before: probe,
        // set the gain, probe the SAME position again.
        tap->requestPage( pageStart, nullptr, 0, CAP, project->getSRate(), nullptr );
    }

    twLevelSample s;
    const bool got = probe.advanceTo( pos, s );

    if( expectMiss_ ) {
        if( got ) {
            qWarning() << "assert-meter FAILED: expected a miss at" << pos
                       << "but measured peak" << s.peak;
            return { false, nullptr };
        }
        return { true, nullptr };
    }

    if( !got ) {
        qWarning() << "assert-meter FAILED: no frozen page for position" << pos
                   << "(page" << pageStart << ")";
        return { false, nullptr };
    }

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
        const int longSide = 120, shortSide = 10;
        if( !win->grabLevelMeter( out, s.peak, rms, grabVertical_,
                                  grabVertical_ ? shortSide : longSide,
                                  grabVertical_ ? longSide : shortSide ) ) {
            qWarning() << "assert-meter FAILED: could not paint the meter into"
                       << out;
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
        const QString desc = win->describeTrackMeter( lanePath, headHeight_ );
        if( desc.isEmpty() ) {
            qWarning() << "assert-meter FAILED: no meter description for track"
                       << lanePath;
            return { false, nullptr };
        }
        if( !desc.contains( contains_ ) ) {
            qWarning() << "assert-meter FAILED: head at height" << headHeight_
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
    return true;
}

static const bool s_reg_assertmeter =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-meter" ),
          [] { return new SAssertMeterAction; } ),
      true );
