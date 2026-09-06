#include "app/testkit/sassertmasterinputsaction.h"

#include <QDebug>
#include <QDomElement>
#include <cmath>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"

SApplyResult SAssertMasterInputsAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SStdMixer *mixer = dynamic_cast<SStdMixer *>(
        splacements::rootNamed( project, arrangement_ ) );
    if( !mixer ) {
        qWarning() << "assert-master-inputs: no mixer root for arrangement"
                   << arrangement_;
        return { false, nullptr };
    }
    // THROUGH SStdMixer's OWN ACCESSORS, not off the twMixer: `app/testkit`
    // may not include `tw/mix` (tools/check_layering.py enforces it, and it
    // caught this verb's first draft). Bus 0 is the bus wireMasterChain()
    // feeds into the master lane's chain (D3), and the mixer picks it.
    bool ok = true;
    const int n = mixer->masterInputCount();
    if( n < 0 ) {
        qWarning() << "assert-master-inputs: no bus-0 mixer";
        return { false, nullptr };
    }

    if( count_ >= 0 && n != count_ ) {
        qWarning() << "assert-master-inputs FAILED: the master sum has" << n
                   << "input(s), expected" << count_
                   << "- a reconnectTracksToMixer pass that forgot the send"
                      " lanes sets this back to the TRACK count (proposal 45"
                      " trap T3)";
        ok = false;
    }

    if( allUnity_.startsWith( '1' ) || allUnity_.startsWith( 't' ) ) {
        for( int i = 0; i < n; ++i ) {
            const double db = mixer->masterInputLevelDb( i );
            if( std::fabs( db ) > 1e-9 ) {
                qWarning() << "assert-master-inputs FAILED: input" << i
                           << "is at" << db << "dB, not unity -"
                           << "twlive::checkMasterShape refuses the LINEAR"
                              " master split on any non-unity input, so this"
                              " silently drops live monitoring into the"
                              " Closure path for every armed track";
                ok = false;
            }
        }
    }

    if( wired_ >= 0 ) {
        int wired = 0;
        for( int i = 0; i < n; ++i )
            if( mixer->masterInputWired( i ) ) ++wired;
        if( wired != wired_ ) {
            qWarning() << "assert-master-inputs FAILED:" << wired
                       << "input(s) have a source, expected" << wired_;
            ok = false;
        }
    }

    if( ok )
        qDebug() << "assert-master-inputs: OK -" << n << "input(s)";
    return { ok, nullptr };
}

void SAssertMasterInputsAction::writeXml( QDomElement &elem ) const
{
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
    if( count_ >= 0 )             elem.setAttribute( "count", count_ );
    if( !allUnity_.isEmpty() )    elem.setAttribute( "allUnity", allUnity_ );
    if( wired_ >= 0 )             elem.setAttribute( "wired", wired_ );
}

bool SAssertMasterInputsAction::readXml( const QDomElement &elem, int )
{
    arrangement_ = elem.attribute( "arrangement" );
    count_       = elem.attribute( "count", "-1" ).toInt();
    allUnity_    = elem.attribute( "allUnity" );
    wired_       = elem.attribute( "wired", "-1" ).toInt();
    return true;
}

static const bool s_reg_assert_master_inputs = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-master-inputs" ),
        []{ return new SAssertMasterInputsAction; } ), true );
