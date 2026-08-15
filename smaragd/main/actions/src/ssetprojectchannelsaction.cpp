#include "app/actions/ssetprojectchannelsaction.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"

#include <QDomElement>

SSetProjectChannelsAction::SSetProjectChannelsAction( int channels )
    : channels_( channels )
{
}

SApplyResult SSetProjectChannelsAction::apply( SProject *project )
{
    if( !project ) {
        return { false, nullptr };
    }
    // Validate BEFORE mutating: an invalid width is a rejection, not a clamp.
    // SProject::setChannels() would refuse it anyway, but a silent no-op that
    // still reported applied=true would hand the undo stack an inverse for a
    // mutation that never happened.
    if( !SProject::isValidChannelCount( channels_ ) ) {
        return { false, nullptr };
    }

    const int oldChannels = project->channels();
    if( oldChannels == channels_ ) {
        // Nothing to undo. Applied, because the requested state IS the state —
        // a script asserting on the result should not see a rejection.
        return { true, nullptr };
    }

    project->setChannels( channels_ );

    // Proposal 36 M1: this is the whole apply(). No bus count, no mixer width,
    // no tw303aEnvironment. See the header for why the omission is deliberate
    // and how it is gated.
    return { true, new SSetProjectChannelsAction( oldChannels ) };
}

QString SSetProjectChannelsAction::mergeKey() const
{
    // One channel count per project, so a constant key: there is nothing to
    // disambiguate and two consecutive changes are always the same control.
    return QStringLiteral("set-project-channels");
}

bool SSetProjectChannelsAction::mergeWith( const SAction *later )
{
    const SSetProjectChannelsAction *o =
        dynamic_cast<const SSetProjectChannelsAction*>( later );
    if( !o ) return false;
    // Absorb the newer target; keep our own (older) value as the baseline the
    // inverse will be captured against.
    channels_ = o->channels_;
    return true;
}

void SSetProjectChannelsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "channels", channels_ );
}

bool SSetProjectChannelsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    // Sniff the spelling rather than key off formatVersion(): .qxa scripts carry
    // no version attribute unless the verb bumped past 1, and this verb is at 1.
    // An absent or unparsable attribute reads as 2 — the default project width —
    // and a value outside the supported set survives readXml so that apply()
    // is the single place that REJECTS it. Rejecting here instead would make an
    // invalid element unloadable and therefore untestable with expectReject.
    channels_ = elem.attribute( "channels", "2" ).toInt();
    return true;
}

QStringList SSetProjectChannelsAction::knownAttributes() const
{
    return { QStringLiteral("channels") };
}

static const bool s_reg_setprojectchannels = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-project-channels"),
        []{ return new SSetProjectChannelsAction; } ),
    true
);
