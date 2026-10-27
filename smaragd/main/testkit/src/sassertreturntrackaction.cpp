#include "app/testkit/sassertreturntrackaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/objects/track/strack.h"

namespace {
bool truthy( const QString &s ) { return s.startsWith( '1' ) || s.startsWith( 't' ); }
}

SApplyResult SAssertReturnTrackAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    QString root;
    STrack *ret = dynamic_cast<STrack *>(
        splacements::laneBySpec( project, trackPath_, root ) );
    if( !ret ) {
        qWarning().noquote() << "assert-return-track FAILED: no track at" << trackPath_;
        return { false, nullptr };
    }

    bool ok = true;
    auto fail = [&]( const QString &what, const QString &got, const QString &want ) {
        qWarning().noquote() << "assert-return-track FAILED [" << trackPath_ << "]"
                             << what << ": got" << got << "want" << want;
        ok = false;
    };

    STrack *src = ret->auxSourceTrack();
    if( !isReturn_.isEmpty() && ret->isReturnTrack() != truthy( isReturn_ ) )
        fail( "isReturn", ret->isReturnTrack() ? "1" : "0", isReturn_ );
    if( !source_.isEmpty() ) {
        QString r2 = root;
        SObject *want = splacements::laneBySpec( project, source_, r2 );
        if( !want || static_cast<SObject *>( src ) != want )
            fail( "source", src ? src->getSName() : QStringLiteral( "(none)" ),
                  want ? want->getSName() : source_ + QStringLiteral( " (unresolved)" ) );
    }
    const int slotIdx = src ? src->pluginSlotIndexOf( ret->auxSourceSlotId() ) : -1;
    if( slot_ >= 0 && slotIdx != slot_ )
        fail( "slot", QString::number( slotIdx ), QString::number( slot_ ) );
    if( bus_ >= 0 && ret->auxSourceBus() != bus_ )
        fail( "bus", QString::number( ret->auxSourceBus() ), QString::number( bus_ ) );
    if( !routed_.isEmpty() && ret->returnRouted() != truthy( routed_ ) )
        fail( "routed", ret->returnRouted() ? "1" : "0", routed_ );
    const QString tap = ret->describeReturnTap();
    if( !mapping_.isEmpty() && !tap.contains( mapping_ ) )
        fail( "mapping", tap.isEmpty() ? QStringLiteral( "(no tap)" ) : tap, mapping_ );
    if( mask_ >= 0 ) {
        const int m = ( src && slotIdx >= 0 ) ? (int) src->pluginSlotRoutedMask( slotIdx ) : -1;
        if( m != mask_ ) fail( "mask", QString::number( m ), QString::number( mask_ ) );
    }

    if( ok )
        qInfo().noquote() << "assert-return-track OK [" << trackPath_ << "] return="
                          << ( ret->isReturnTrack() ? 1 : 0 ) << "routed="
                          << ( ret->returnRouted() ? 1 : 0 ) << "slot=" << slotIdx
                          << "bus=" << ret->auxSourceBus() << tap;
    return { ok, nullptr };
}

void SAssertReturnTrackAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    if( !isReturn_.isEmpty() ) elem.setAttribute( "isReturn", isReturn_ );
    if( !source_.isEmpty() )   elem.setAttribute( "source", source_ );
    if( slot_ >= 0 )           elem.setAttribute( "slot", slot_ );
    if( bus_ >= 0 )            elem.setAttribute( "bus", bus_ );
    if( !routed_.isEmpty() )   elem.setAttribute( "routed", routed_ );
    if( !mapping_.isEmpty() )  elem.setAttribute( "mapping", mapping_ );
    if( mask_ >= 0 )           elem.setAttribute( "mask", mask_ );
}

bool SAssertReturnTrackAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    isReturn_  = elem.attribute( "isReturn" );
    source_    = elem.attribute( "source" );
    slot_      = elem.hasAttribute( "slot" ) ? elem.attribute( "slot" ).toInt() : -1;
    bus_       = elem.hasAttribute( "bus" ) ? elem.attribute( "bus" ).toInt() : -1;
    routed_    = elem.attribute( "routed" );
    mapping_   = elem.attribute( "mapping" );
    mask_      = elem.hasAttribute( "mask" ) ? elem.attribute( "mask" ).toInt() : -1;
    return !trackPath_.isEmpty();
}

static const bool s_reg_assert_return_track = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-return-track" ),
        []{ return new SAssertReturnTrackAction; } ), true );
