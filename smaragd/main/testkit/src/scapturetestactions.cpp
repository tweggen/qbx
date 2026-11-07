#include "app/testkit/scapturetestactions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"

namespace {
SCut *cutNamed( SProject *project, const QString &verb, const QString &name )
{
    SCut *cut = project ? dynamic_cast<SCut *>( project->asset( name ) ) : nullptr;
    if( !cut ) qWarning().noquote() << verb << "FAILED: there is no asset" << name;
    return cut;
}
}  // namespace

SApplyResult SSetCaptureGateAction::apply( SProject *project )
{
    SCut *cut = cutNamed( project, name(), assetName_ );
    if( !cut ) return { false, nullptr };
    cut->setCaptureGateReady( ready_, holdMs_ );
    qInfo().noquote() << "set-capture-gate:" << assetName_ << "ready=" << ( ready_ ? 1 : 0 );
    return { true, nullptr };
}

void SSetCaptureGateAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "assetName", assetName_ );
    elem.setAttribute( "ready", ready_ ? "true" : "false" );
    if( holdMs_ > 0 ) elem.setAttribute( "holdMs", holdMs_ );
}

bool SSetCaptureGateAction::readXml( const QDomElement &elem, int )
{
    assetName_ = elem.attribute( "assetName" );
    ready_ = elem.attribute( "ready", "true" ) == QLatin1String( "true" );
    holdMs_ = elem.attribute( "holdMs", "0" ).toInt();
    return !assetName_.isEmpty();
}

SApplyResult SAssertCaptureHoldAction::apply( SProject *project )
{
    SCut *cut = cutNamed( project, name(), assetName_ );
    if( !cut ) return { false, nullptr };
    const bool held = cut->liveHeld();
    const QString heldFor = cut->liveHeldFor();
    bool ok = true;
    if( !held_.isEmpty() && ( held_.startsWith( '1' ) || held_.startsWith( 't' ) ) != held )
        ok = false;
    if( checkHeldFor_ && heldFor != heldFor_ ) ok = false;
    const QString got = QStringLiteral( "held=%1 heldFor=%2" ).arg( held ? 1 : 0 ).arg( heldFor );
    if( !ok )
        qWarning().noquote() << "assert-capture-hold FAILED [" << assetName_ << "]: want held="
                             << held_ << "heldFor=" << heldFor_ << "; got" << got;
    else
        qInfo().noquote() << "assert-capture-hold OK [" << assetName_ << "]" << got;
    return { ok, nullptr };
}

void SAssertCaptureHoldAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "assetName", assetName_ );
    if( !held_.isEmpty() ) elem.setAttribute( "held", held_ );
    if( checkHeldFor_ ) elem.setAttribute( "heldFor", heldFor_ );
}

bool SAssertCaptureHoldAction::readXml( const QDomElement &elem, int )
{
    assetName_    = elem.attribute( "assetName" );
    held_         = elem.attribute( "held" );
    checkHeldFor_ = elem.hasAttribute( "heldFor" );
    heldFor_      = elem.attribute( "heldFor" );
    return !assetName_.isEmpty() && ( !held_.isEmpty() || checkHeldFor_ );
}

static const bool s_reg_set_capture_gate = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-capture-gate" ),
        []{ return new SSetCaptureGateAction; } ), true );
static const bool s_reg_assert_capture_hold = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-capture-hold" ),
        []{ return new SAssertCaptureHoldAction; } ), true );
