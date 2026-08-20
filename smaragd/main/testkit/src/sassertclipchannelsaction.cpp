#include "app/testkit/sassertclipchannelsaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"

#include "tw/core/twtypes.h"
#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"
#include "tw/schedule/capture_revalidator.h"

#include <QDebug>
#include <QDomElement>
#include <cmath>

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

}  // namespace

SApplyResult SAssertClipChannelsAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        qWarning() << "assert-clip-channels: no clip path";
        return { false, nullptr };
    }

    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = mixer ? splacements::placementAt( mixer, clipPath_ ) : nullptr;
    if( !link ) {
        qWarning() << "assert-clip-channels: no clip at path"
                   << qualifiedToString( pathRoot_, clipPath_ );
        return { false, nullptr };
    }
    SCut *cut = dynamic_cast<SCut *>( &link->getSObject() );
    if( !cut ) {
        qWarning() << "assert-clip-channels: target is not an SCut at path"
                   << qualifiedToString( pathRoot_, clipPath_ );
        return { false, nullptr };
    }

    // THE RESOLUTION CHAIN, exactly as twView::freezePage walks it: one snapshot
    // yielding both the component and the clip-relative -> component-domain
    // position (proposal 19 Inv-1). Asserting on the component this returns is
    // what makes AC B3.2 about the chain rather than about the reader.
    twResolvedClip r = cut->resolveClip( (offset_t) position_ );
    if( !r.component ) {
        qWarning() << "assert-clip-channels: clip" << qualifiedToString( pathRoot_, clipPath_ )
                   << "resolves to no component at" << (qlonglong) position_;
        return { false, nullptr };
    }

    const offset_t CAP = (offset_t) twOutputPage::FRAME_CAPACITY;
    const offset_t pageStart = twFloorAlign( r.mappedPos, CAP );

    // THROUGH THE REAL SCHEDULER. A demand on the project's CaptureRevalidator
    // is the same route the playback readahead and the offline render take: the
    // node is planned, dependency-counted and executed on a worker, and the page
    // lands in the component's own cache.
    CaptureRevalidator *reval = project->getRevalidator();
    if( !reval ) {
        qWarning() << "assert-clip-channels: no revalidator "
                      "(SMARAGD_REVAL_WORKERS=0 disables the scheduler this verb "
                      "exists to exercise)";
        return { false, nullptr };
    }
    auto demand = reval->requestGraphPages( r.component, pageStart, 1 );
    if( demand ) demand->wait();

    std::shared_ptr<twOutputPage> page = r.component->getPageIfExists( pageStart );
    if( !page || page->validAspects.load() == 0 || page->validFrames == 0 ) {
        qWarning() << "assert-clip-channels: no frozen page at" << (qlonglong) pageStart
                   << "for clip" << qualifiedToString( pathRoot_, clipPath_ )
                   << "(clip-relative" << (qlonglong) position_
                   << "-> component" << (qlonglong) r.mappedPos << ")";
        return { false, nullptr };
    }

    const int nCh = (int) page->channels();
    const QString where =
        QString( "clip %1 @ %2 (component pos %3, page %4): %5 channels, "
                 "%6 valid frames" )
            .arg( qualifiedToString( pathRoot_, clipPath_ ) ).arg( (qlonglong) position_ )
            .arg( (qlonglong) r.mappedPos ).arg( (qlonglong) pageStart )
            .arg( nCh ).arg( page->validFrames );

    if( expectChannels_ > 0 && nCh != expectChannels_ ) {
        qWarning() << "assert-clip-channels FAILED:" << where
                   << "- expected" << expectChannels_ << "channels";
        return { false, nullptr };
    }

    if( channelA_ < 0 || channelB_ < 0 || channelA_ >= nCh || channelB_ >= nCh ) {
        // Never a silent pass on a channel the page does not have: an
        // out-of-range channelPtr() clamps to 0, which would compare a channel
        // with itself and read as "identical" rather than as a mistake.
        qWarning() << "assert-clip-channels FAILED:" << where
                   << "- channel" << channelA_ << "/" << channelB_
                   << "out of range";
        return { false, nullptr };
    }
    if( channelA_ == channelB_ ) {
        qWarning() << "assert-clip-channels FAILED:" << where
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

    // Both numbers in every message, for the same reason assert-channels-differ
    // prints both: "same level" and "same samples" are different findings.
    const QString measured =
        QString( "%1; ch%2 rms %3, ch%4 rms %5, delta %6, rms(A-B) %7" )
            .arg( where ).arg( channelA_ ).arg( rmsA, 0, 'f', 6 )
            .arg( channelB_ ).arg( rmsB, 0, 'f', 6 )
            .arg( delta, 0, 'f', 6 ).arg( rmsDiff, 0, 'f', 6 );

    if( delta < minRmsDelta_ ) {
        qWarning() << "assert-clip-channels FAILED:" << measured
                   << "- level delta below minRmsDelta" << minRmsDelta_;
        return { false, nullptr };
    }
    if( minDiffRms_ >= 0.0 && rmsDiff < minDiffRms_ ) {
        qWarning() << "assert-clip-channels FAILED:" << measured
                   << "- difference-signal RMS below minDiffRms" << minDiffRms_;
        return { false, nullptr };
    }

    qDebug() << "assert-clip-channels:" << measured << "OK";
    return { true, nullptr };   // an assertion has nothing to undo
}

void SAssertClipChannelsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", qualifiedToString( pathRoot_, clipPath_ ) );
    elem.setAttribute( "position", QString::number( (qlonglong) position_ ) );
    elem.setAttribute( "expectChannels", QString::number( expectChannels_ ) );
    elem.setAttribute( "channelA", QString::number( channelA_ ) );
    elem.setAttribute( "channelB", QString::number( channelB_ ) );
    elem.setAttribute( "minRmsDelta", QString::number( minRmsDelta_, 'f', 6 ) );
    if( minDiffRms_ >= 0.0 ) {
        elem.setAttribute( "minDiffRms", QString::number( minDiffRms_, 'f', 6 ) );
    }
}

bool SAssertClipChannelsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // An empty path is reported by apply(), not here: a readXml that fails makes
    // the action undeserializable and breaks the round-trip audit, which feeds
    // every verb a DEFAULT instance through write->read->write.
    clipPath_       = parseInto( pathRoot_, elem.attribute( "clip" ) );
    position_       = elem.attribute( "position", "0" ).toLongLong();
    expectChannels_ = elem.attribute( "expectChannels", "0" ).toInt();
    channelA_       = elem.attribute( "channelA", "0" ).toInt();
    channelB_       = elem.attribute( "channelB", "1" ).toInt();
    minRmsDelta_    = elem.attribute( "minRmsDelta", "0.01" ).toDouble();
    minDiffRms_     = elem.attribute( "minDiffRms", "-1" ).toDouble();
    return true;
}

static const bool s_reg_assert_clip_channels =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-clip-channels" ),
          [] { return new SAssertClipChannelsAction; } ),
      true );
