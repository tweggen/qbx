#include "app/actions/ssetprojectchannelsaction.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "tw/core/twlog.h"

#include <QSet>

#include <QDomElement>

namespace {

// Every object reachable from the master root, every arrangement root and
// every asset body that carries a NON-ZERO pan, each counted once however many
// placements share it (an asset placed twice is one object, proposal 41 D2).
void countPanned( SObject *obj, QSet<const SObject*> &seen, int &panned )
{
    if( !obj || seen.contains( obj ) ) return;
    seen.insert( obj );
    if( obj->getPan() != 0.0 ) ++panned;
    for( SLink *lk : obj->childLinks() )
        if( lk ) countPanned( &lk->getSObject(), seen, panned );
}

int pannedObjectCount( SProject *project )
{
    QSet<const SObject*> seen;
    int panned = 0;
    countPanned( splacements::rootNamed( project, QString() ), seen, panned );
    for( SObject *root : project->arrangements() )
        countPanned( root, seen, panned );
    for( const QString &name : project->assetNames() )
        countPanned( project->asset( name ), seen, panned );
    return panned;
}

} // namespace

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

    // A STORED PAN THAT BECOMES INERT IS ANNOUNCED, NEVER SILENT (proposal 49
    // D1, trap T12). Pan is defined at width 2 only, so leaving 2 makes every
    // non-zero pan inaudible. The change is NOT refused -- a width change
    // must not be blocked by a pan somewhere in the project -- and the values
    // are kept, heard again if the width returns to 2. Logged once per change,
    // never per clip.
    if( oldChannels == 2 ) {
        const int panned = pannedObjectCount( project );
        if( panned > 0 ) {
            TW_LOGW( "project", "set-project-channels: %d object(s) carry a "
                                "non-zero pan that is now inert at %d channels "
                                "(pan is defined for 2 channels only; the "
                                "values are kept)",
                     panned, channels_ );
        }
    }

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
