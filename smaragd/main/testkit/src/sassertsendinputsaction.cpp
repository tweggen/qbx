#include "app/testkit/sassertsendinputsaction.h"

#include <QDebug>
#include <QDomElement>
#include <cmath>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

SApplyResult SAssertSendInputsAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    SObject *root = splacements::rootNamed( project, arrangement_ );
    SStdMixer *mixer = dynamic_cast<SStdMixer *>( root );
    if( !mixer ) {
        qWarning().noquote() << "assert-send-inputs FAILED: no mixer root";
        return { false, nullptr };
    }

    // By NAME or by sentinel, so a case may spell the lane either way.
    int k = mixer->systemLaneIndexNamed( lane_ );
    if( k < 0 && lane_.startsWith( QLatin1String( "$send" ) ) )
        k = lane_.mid( 5 ).toInt();
    if( k < 0 || !mixer->sendLaneAt( k ) ) {
        qWarning().noquote() << "assert-send-inputs FAILED: no send lane" << lane_;
        return { false, nullptr };
    }

    bool ok = true;
    auto fail = [&]( const QString &what, const QString &got,
                     const QString &want ) {
        qWarning().noquote() << "assert-send-inputs FAILED [" << lane_ << "]"
                             << what << ": got" << got << "want" << want;
        ok = false;
    };

    const int n = mixer->sendBusInputCount( k );
    if( count_ >= 0 && n != count_ )
        fail( "count", QString::number( n ), QString::number( count_ ) );

    if( wired_ >= 0 ) {
        int w = 0;
        for( int i = 0; i < n; ++i ) if( mixer->sendBusInputWired( k, i ) ) ++w;
        if( w != wired_ )
            fail( "wired", QString::number( w ), QString::number( wired_ ) );
    }

    if( hasLevel_ && index_ >= 0 ) {
        const double got = mixer->sendBusInputLevelDb( k, index_ );
        if( std::fabs( got - level_ ) > 1e-9 )
            fail( QStringLiteral( "level[%1]" ).arg( index_ ),
                  QString::number( got, 'g', 10 ),
                  QString::number( level_, 'g', 10 ) );
    }

    if( ok ) {
        int w = 0;
        for( int i = 0; i < n; ++i ) if( mixer->sendBusInputWired( k, i ) ) ++w;
        qInfo().noquote() << "assert-send-inputs OK [" << lane_ << "] inputs"
                          << n << "wired" << w;
    }
    return { ok, nullptr };
}

void SAssertSendInputsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "lane", lane_ );
    if( count_ >= 0 )             elem.setAttribute( "count", count_ );
    if( wired_ >= 0 )             elem.setAttribute( "wired", wired_ );
    if( index_ >= 0 )             elem.setAttribute( "index", index_ );
    if( hasLevel_ )               elem.setAttribute( "level", level_ );
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
}

bool SAssertSendInputsAction::readXml( const QDomElement &elem, int )
{
    lane_        = elem.attribute( "lane" );
    arrangement_ = elem.attribute( "arrangement" );
    count_ = elem.hasAttribute( "count" ) ? elem.attribute( "count" ).toInt() : -1;
    wired_ = elem.hasAttribute( "wired" ) ? elem.attribute( "wired" ).toInt() : -1;
    index_ = elem.hasAttribute( "index" ) ? elem.attribute( "index" ).toInt() : -1;
    if( elem.hasAttribute( "level" ) ) {
        level_ = elem.attribute( "level" ).toDouble();
        hasLevel_ = true;
    }
    return !lane_.isEmpty();
}

static const bool s_reg_assert_send_inputs = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-send-inputs" ),
        []{ return new SAssertSendInputsAction; } ), true );
