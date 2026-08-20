#include "app/testkit/sasserttrackchannelsaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/model/ssolorules.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

#include "tw/core/twtypes.h"
#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"
#include "tw/schedule/capture_revalidator.h"

#include <QDebug>
#include <QDomElement>
#include <cmath>
#include <vector>

using namespace strackpath;

namespace {

double rmsOver( const sample_t *p, uint32_t n )
{
    if( n == 0 ) return 0.0;
    double s = 0.0;
    for( uint32_t i = 0; i < n; ++i ) s += (double) p[i] * (double) p[i];
    return std::sqrt( s / (double) n );
}

double diffRmsOver( const sample_t *a, const sample_t *b, uint32_t n )
{
    if( n == 0 ) return 0.0;
    double s = 0.0;
    for( uint32_t i = 0; i < n; ++i ) {
        const double d = (double) a[i] - (double) b[i];
        s += d * d;
    }
    return std::sqrt( s / (double) n );
}

STrack *laneAtPath( SProject *project, const QString &pathRoot_,
                    const QList<int> &path )
{
    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if( !root ) return nullptr;
    SObject *lane = splacements::laneAt( root, path );
    return dynamic_cast<STrack *>( lane );
}

// Materialize `comp`'s page for pageStart, by one of the two drivers.
//
// "scheduler" is the demand path (planned, dependency-counted, executed on a
// worker) and is what playback and the offline render use. "pull" is the LEGACY
// recursive freeze — the path assert-meter drives and the ONLY one available
// when SMARAGD_REVAL_WORKERS=0 has disabled the revalidator entirely, which is
// what AC B4.6 exists to exercise.
std::shared_ptr<twOutputPage> materialize( SProject *project,
                                           const std::shared_ptr<twComponent> &comp,
                                           offset_t pageStart,
                                           bool useScheduler )
{
    if( !comp ) return nullptr;

    CaptureRevalidator *reval = useScheduler ? project->getRevalidator() : nullptr;
    if( reval ) {
        auto demand = reval->requestGraphPages( comp, pageStart, 1 );
        if( demand ) demand->wait();
        std::shared_ptr<twOutputPage> page = comp->getPageIfExists( pageStart );
        if( page && page->validAspects.load() != 0 ) return page;
        // Fall through to the pull: a demand can legitimately come back with
        // nothing (a plan miss), and a verb that reported "no page" there would
        // be measuring the scheduler rather than the width.
    }

    return comp->requestPage( pageStart, nullptr, 0,
                              (length_t) twOutputPage::FRAME_CAPACITY,
                              project->getSRate(), nullptr );
}

}  // namespace

// ---------------------------------------------------------------------------
// assert-track-channels (AC B4.7, and AC B4.6 with driver="pull")

SApplyResult SAssertTrackChannelsAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    STrack *track = laneAtPath( project, pathRoot_, trackPath_ );
    if( !track ) {
        qWarning() << "assert-track-channels: no track at path"
                   << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }

    std::shared_ptr<twComponent> tap = track->getRootComponent();
    if( !tap ) {
        qWarning() << "assert-track-channels: track" << qualifiedToString( pathRoot_, trackPath_ )
                   << "has no root component";
        return { false, nullptr };
    }

    const offset_t CAP = (offset_t) twOutputPage::FRAME_CAPACITY;
    const offset_t pageStart = twFloorAlign( (offset_t) position_, CAP );
    const bool useScheduler = ( driver_.compare( QStringLiteral("pull"),
                                                 Qt::CaseInsensitive ) != 0 );

    std::shared_ptr<twOutputPage> page =
        materialize( project, tap, pageStart, useScheduler );

    if( !page || page->validAspects.load() == 0 || page->validFrames == 0 ) {
        qWarning() << "assert-track-channels: no frozen page at"
                   << (qlonglong) pageStart << "for track"
                   << qualifiedToString( pathRoot_, trackPath_ ) << "(driver" << driver_ << ")";
        return { false, nullptr };
    }

    const int nCh = (int) page->channels();
    const QString where =
        QString( "track %1 @ %2 (page %3, driver %4): %5 channels, %6 valid frames" )
            .arg( qualifiedToString( pathRoot_, trackPath_ ) ).arg( (qlonglong) position_ )
            .arg( (qlonglong) pageStart ).arg( driver_ )
            .arg( nCh ).arg( page->validFrames );

    if( expectChannels_ > 0 && nCh != expectChannels_ ) {
        qWarning() << "assert-track-channels FAILED:" << where
                   << "- expected" << expectChannels_ << "channels";
        return { false, nullptr };
    }

    if( channelA_ < 0 || channelB_ < 0 || channelA_ >= nCh || channelB_ >= nCh ) {
        // Never a silent pass on a channel the page does not have: an
        // out-of-range channelPtr() clamps to 0, which would compare a channel
        // with itself and read as "identical" rather than as a mistake.
        qWarning() << "assert-track-channels FAILED:" << where
                   << "- channel" << channelA_ << "/" << channelB_ << "out of range";
        return { false, nullptr };
    }
    if( channelA_ == channelB_ ) {
        qWarning() << "assert-track-channels FAILED:" << where
                   << "- channelA and channelB are the same channel";
        return { false, nullptr };
    }

    const uint32_t n = page->validFrames;
    const sample_t *a = page->channelPtr( (idx_t) channelA_ );
    const sample_t *b = page->channelPtr( (idx_t) channelB_ );
    const double rmsA = rmsOver( a, n );
    const double rmsB = rmsOver( b, n );
    const double delta = std::fabs( rmsA - rmsB );
    const double rmsDiff = diffRmsOver( a, b, n );

    const QString measured =
        QString( "%1; ch%2 rms %3, ch%4 rms %5, delta %6, rms(A-B) %7" )
            .arg( where ).arg( channelA_ ).arg( rmsA, 0, 'f', 6 )
            .arg( channelB_ ).arg( rmsB, 0, 'f', 6 )
            .arg( delta, 0, 'f', 6 ).arg( rmsDiff, 0, 'f', 6 );

    if( minRms_ >= 0.0 && rmsA < minRms_ ) {
        qWarning() << "assert-track-channels FAILED:" << measured
                   << "- channelA rms below minRms" << minRms_
                   << "(a page of the right width full of silence)";
        return { false, nullptr };
    }
    if( delta < minRmsDelta_ ) {
        qWarning() << "assert-track-channels FAILED:" << measured
                   << "- level delta below minRmsDelta" << minRmsDelta_;
        return { false, nullptr };
    }
    if( minDiffRms_ >= 0.0 && rmsDiff < minDiffRms_ ) {
        qWarning() << "assert-track-channels FAILED:" << measured
                   << "- difference-signal RMS below minDiffRms" << minDiffRms_;
        return { false, nullptr };
    }

    qDebug() << "assert-track-channels:" << measured << "OK";
    return { true, nullptr };   // an assertion has nothing to undo
}

void SAssertTrackChannelsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "position", QString::number( (qlonglong) position_ ) );
    elem.setAttribute( "expectChannels", QString::number( expectChannels_ ) );
    elem.setAttribute( "channelA", QString::number( channelA_ ) );
    elem.setAttribute( "channelB", QString::number( channelB_ ) );
    elem.setAttribute( "minRmsDelta", QString::number( minRmsDelta_, 'f', 6 ) );
    if( minDiffRms_ >= 0.0 )
        elem.setAttribute( "minDiffRms", QString::number( minDiffRms_, 'f', 6 ) );
    if( minRms_ >= 0.0 )
        elem.setAttribute( "minRms", QString::number( minRms_, 'f', 6 ) );
    elem.setAttribute( "driver", driver_ );
}

bool SAssertTrackChannelsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_      = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    position_       = elem.attribute( "position", "0" ).toLongLong();
    expectChannels_ = elem.attribute( "expectChannels", "0" ).toInt();
    channelA_       = elem.attribute( "channelA", "0" ).toInt();
    channelB_       = elem.attribute( "channelB", "1" ).toInt();
    minRmsDelta_    = elem.attribute( "minRmsDelta", "0.01" ).toDouble();
    minDiffRms_     = elem.attribute( "minDiffRms", "-1" ).toDouble();
    minRms_         = elem.attribute( "minRms", "-1" ).toDouble();
    driver_         = elem.attribute( "driver", QStringLiteral("scheduler") );
    return true;
}

static const bool s_reg_assert_track_channels =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-track-channels" ),
          [] { return new SAssertTrackChannelsAction; } ),
      true );

// ---------------------------------------------------------------------------
// assert-master-sums (AC B4.1)

SApplyResult SAssertMasterSumsAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SStdMixer *mixer = dynamic_cast<SStdMixer *>( root );
    if( !mixer ) {
        qWarning() << "assert-master-sums: the project root is not an SStdMixer";
        return { false, nullptr };
    }

    const offset_t CAP = (offset_t) twOutputPage::FRAME_CAPACITY;
    const offset_t pageStart = twFloorAlign( (offset_t) position_, CAP );

    std::shared_ptr<twOutputPage> master =
        materialize( project, mixer->getRootComponent(), pageStart, true );
    if( !master || master->validAspects.load() == 0 || master->validFrames == 0 ) {
        qWarning() << "assert-master-sums: no master page at" << (qlonglong) pageStart;
        return { false, nullptr };
    }

    // The AUDIBLE tracks only: the mixer excludes a muted or soloed-out lane by
    // nulling its input plug, so including it here would make the sum wrong for
    // a reason that has nothing to do with channels.
    const bool anySolo = ssolo::anySoloInTree( mixer );
    std::vector<std::shared_ptr<twOutputPage>> trackPages;
    for( SLink *lk : mixer->childLinks() ) {
        if( !lk ) continue;
        SObject &so = lk->getSObject();
        if( !ssolo::isLaneAudible( mixer, &so, anySolo ) ) continue;
        std::shared_ptr<twComponent> comp = lk->getRootComponent();
        if( !comp ) continue;
        std::shared_ptr<twOutputPage> p = materialize( project, comp, pageStart, true );
        if( !p || p->validAspects.load() == 0 ) {
            qWarning() << "assert-master-sums: a track produced no page at"
                       << (qlonglong) pageStart;
            return { false, nullptr };
        }
        trackPages.push_back( std::move( p ) );
    }

    if( trackPages.empty() ) {
        qWarning() << "assert-master-sums: no audible tracks — nothing to sum";
        return { false, nullptr };
    }

    const idx_t nCh = (idx_t) master->channels();
    const uint32_t n = master->validFrames;
    const int stride = stride_ > 0 ? stride_ : 1;

    if( minRms_ > 0.0 ) {
        const double r = rmsOver( master->channelPtr( 0 ), n );
        if( r < minRms_ ) {
            qWarning() << "assert-master-sums FAILED: master channel 0 rms" << r
                       << "< minRms" << minRms_
                       << "- a silent master sums to a silent sum trivially";
            return { false, nullptr };
        }
    }

    double worst = 0.0;
    int worstCh = -1;
    uint32_t worstFrame = 0;
    for( idx_t c = 0; c < nCh; ++c ) {
        const sample_t *m = master->channelPtr( c );
        for( uint32_t i = 0; i < n; i += (uint32_t) stride ) {
            double sum = 0.0;
            for( const std::shared_ptr<twOutputPage> &p : trackPages ) {
                if( i >= p->validFrames ) continue;
                // §4.4's clamp: a NARROWER track contributes its last channel to
                // every channel above it, which is exactly what the master's own
                // renderPageWide does.
                sum += (double) p->channelPtr( twPageClampChannel( *p, c ) )[i];
            }
            const double d = std::fabs( (double) m[i] - sum );
            if( d > worst ) { worst = d; worstCh = (int) c; worstFrame = i; }
        }
    }

    const QString measured =
        QString( "master @ %1 (page %2): %3 channels, %4 tracks, worst |master - "
                 "sum| = %5 on channel %6 frame %7" )
            .arg( (qlonglong) position_ ).arg( (qlonglong) pageStart )
            .arg( (int) nCh ).arg( (int) trackPages.size() )
            .arg( worst, 0, 'g', 6 ).arg( worstCh ).arg( worstFrame );

    if( worst > tolerance_ ) {
        qWarning() << "assert-master-sums FAILED:" << measured
                   << "- exceeds tolerance" << tolerance_;
        return { false, nullptr };
    }

    qDebug() << "assert-master-sums:" << measured << "OK";
    return { true, nullptr };
}

void SAssertMasterSumsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "position", QString::number( (qlonglong) position_ ) );
    elem.setAttribute( "tolerance", QString::number( tolerance_, 'g', 6 ) );
    elem.setAttribute( "stride", QString::number( stride_ ) );
    elem.setAttribute( "minRms", QString::number( minRms_, 'f', 6 ) );
}

bool SAssertMasterSumsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    position_  = elem.attribute( "position", "0" ).toLongLong();
    tolerance_ = elem.attribute( "tolerance", "0.0005" ).toDouble();
    stride_    = elem.attribute( "stride", "97" ).toInt();
    minRms_    = elem.attribute( "minRms", "0" ).toDouble();
    return true;
}

static const bool s_reg_assert_master_sums =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-master-sums" ),
          [] { return new SAssertMasterSumsAction; } ),
      true );
