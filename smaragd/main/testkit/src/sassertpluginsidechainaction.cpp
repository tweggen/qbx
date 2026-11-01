#include "app/testkit/sassertpluginsidechainaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/objects/track/strack.h"

namespace {
bool truthy( const QString &s ) { return s.startsWith( '1' ) || s.startsWith( 't' ); }
}

SApplyResult SAssertPluginSidechainAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    QString root;
    STrack *t = dynamic_cast<STrack *>(
        splacements::laneBySpec( project, trackPath_, root ) );
    if( !t ) {
        qWarning().noquote() << "assert-plugin-sidechain FAILED: no track at" << trackPath_;
        return { false, nullptr };
    }
    if( slot_ < 0 || slot_ >= t->pluginSlotCount() ) {
        qWarning().noquote() << "assert-plugin-sidechain FAILED: no slot" << slot_
                             << "on" << trackPath_;
        return { false, nullptr };
    }

    bool ok = true;
    auto fail = [&]( const QString &what, const QString &got, const QString &want ) {
        qWarning().noquote() << "assert-plugin-sidechain FAILED [" << trackPath_
                             << "slot" << slot_ << "]" << what << ": got" << got
                             << "want" << want;
        ok = false;
    };

    const bool hasKey = t->pluginSlotHasSidechain( slot_ );
    SObject *src = t->pluginSlotSidechainSource( slot_ );
    const bool pre = t->pluginSlotSidechainPre( slot_ );
    const bool wired = t->pluginSlotKeyWired( slot_ );
    const QString desc = t->describePluginSlotKey( slot_ );

    if( !keyed_.isEmpty() && hasKey != truthy( keyed_ ) )
        fail( "keyed", hasKey ? "1" : "0", keyed_ );
    if( !source_.isEmpty() ) {
        QString r2 = root;
        SObject *want = splacements::laneBySpec( project, source_, r2 );
        if( !want || src != want )
            fail( "source", src ? src->getSName() : QStringLiteral( "(none)" ),
                  want ? want->getSName() : source_ + QStringLiteral( " (unresolved)" ) );
    }
    if( !tap_.isEmpty() ) {
        const QString got = pre ? QStringLiteral( "pre" ) : QStringLiteral( "post" );
        if( got != tap_ ) fail( "tap", got, tap_ );
    }
    if( !wired_.isEmpty() && wired != truthy( wired_ ) )
        fail( "wired", wired ? "1" : "0", wired_ );
    if( !mapping_.isEmpty() && !desc.contains( mapping_ ) )
        fail( "mapping", desc.isEmpty() ? QStringLiteral( "(no processor)" ) : desc,
              mapping_ );

    if( ok )
        qInfo().noquote() << "assert-plugin-sidechain OK [" << trackPath_ << "slot"
                          << slot_ << "] keyed=" << ( hasKey ? 1 : 0 ) << "source="
                          << ( src ? src->getSName() : QStringLiteral( "(none)" ) )
                          << "tap=" << ( pre ? "pre" : "post" ) << "wired="
                          << ( wired ? 1 : 0 ) << desc;
    return { ok, nullptr };
}

void SAssertPluginSidechainAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slot", slot_ );
    if( !keyed_.isEmpty() )   elem.setAttribute( "keyed", keyed_ );
    if( !source_.isEmpty() )  elem.setAttribute( "source", source_ );
    if( !tap_.isEmpty() )     elem.setAttribute( "tap", tap_ );
    if( !wired_.isEmpty() )   elem.setAttribute( "wired", wired_ );
    if( !mapping_.isEmpty() ) elem.setAttribute( "mapping", mapping_ );
}

bool SAssertPluginSidechainAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    slot_      = elem.attribute( "slot", "0" ).toInt();
    keyed_     = elem.attribute( "keyed" );
    source_    = elem.attribute( "source" );
    tap_       = elem.attribute( "tap" );
    wired_     = elem.attribute( "wired" );
    mapping_   = elem.attribute( "mapping" );
    return !trackPath_.isEmpty();
}

static const bool s_reg_assert_plugin_sidechain = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-plugin-sidechain" ),
        []{ return new SAssertPluginSidechainAction; } ), true );
