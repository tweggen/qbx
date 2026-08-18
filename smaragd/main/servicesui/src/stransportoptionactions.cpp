#include "app/servicesui/stransportoptionactions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/servicesui/soptions.h"
#include "app/shell/ssettings.h"

QString STransportBarsAction::name() const
{
    return preRoll_ ? QStringLiteral( "set-pre-roll" )
                    : QStringLiteral( "set-count-in" );
}

QStringList STransportBarsAction::knownAttributes() const
{
    return { QStringLiteral( "bars" ) };
}

SApplyResult STransportBarsAction::apply( SProject * )
{
    if( bars_ < 0 || bars_ > kMaxBars ) {
        qWarning() << name() << ": bars must be 0.." << kMaxBars
                   << "- refusing" << bars_;
        return { false, nullptr };
    }
    SSettings::instance().setValue(
        preRoll_ ? QString::fromLatin1( SOpt::PreRollBars )
                 : QString::fromLatin1( SOpt::CountInBars ),
        bars_ );
    // NOT undoable: a per-user preference, exactly as `set-option` is.
    return { true, nullptr };
}

void STransportBarsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "bars", QString::number( bars_ ) );
}

bool STransportBarsAction::readXml( const QDomElement &elem, int )
{
    bars_ = elem.attribute( "bars", "0" ).toInt();
    return true;
}

static const bool s_reg_transport_bars = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-count-in" ),
        []{ return new STransportBarsAction( /*preRoll=*/false ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-pre-roll" ),
        []{ return new STransportBarsAction( /*preRoll=*/true ); } ),
    true
);
