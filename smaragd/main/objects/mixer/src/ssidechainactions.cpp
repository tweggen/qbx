#include "app/objects/mixer/ssidechainactions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

namespace {

/// THE ONE LANE RESOLVER, so both verbs refuse alike. `rootInOut` carries the
/// arrangement root (SAction::pathRoot_ for the keyed track, a copy of it for
/// the source -- never a shadow of it).
STrack *resolveLane( SProject *project, const QString &verb, const QString &spec,
                     QString &rootInOut, SStdMixer **mixerOut, const char *what )
{
    SObject *rootObj = nullptr;
    SObject *obj = splacements::laneBySpec( project, spec, rootInOut, &rootObj );
    STrack *t = dynamic_cast<STrack *>( obj );
    if( !t ) {
        qWarning().noquote() << verb << ": refused, no lane at" << spec
                             << "-- the" << what << "is addressed like any other lane";
        return nullptr;
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer *>( rootObj );
    if( !mixer ) {
        qWarning().noquote() << verb << ": refused, no mixer root for" << spec;
        return nullptr;
    }
    if( mixerOut ) *mixerOut = mixer;
    return t;
}

}  // namespace

// --- set-plugin-sidechain ---------------------------------------------------

SSetPluginSidechainAction::SSetPluginSidechainAction( const QString &trackPath, int slot,
                                                      const QString &source,
                                                      const QString &tap )
    : trackPath_( trackPath ), slot_( slot ), source_( source ), tap_( tap )
{
}

SApplyResult SSetPluginSidechainAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    const QString tap = tap_.isEmpty() ? QStringLiteral( "pre" ) : tap_;
    if( tap != QLatin1String( "pre" ) && tap != QLatin1String( "post" ) ) {
        qWarning().noquote() << name() << ": refused, tap must be 'pre' or 'post', not"
                             << tap_;
        return { false, nullptr };
    }

    SStdMixer *mixer = nullptr;
    STrack *keyed = resolveLane( project, name(), trackPath_, pathRoot_, &mixer,
                                 "keyed track" );
    if( !keyed ) return { false, nullptr };
    QString srcRoot = pathRoot_;
    SStdMixer *srcMixer = nullptr;
    STrack *source = resolveLane( project, name(), source_, srcRoot, &srcMixer,
                                  "key source" );
    if( !source ) return { false, nullptr };
    if( srcMixer != mixer ) {
        qWarning().noquote()
            << name() << ": refused, the key source" << source_
            << "is in another arrangement than the keyed track" << trackPath_;
        return { false, nullptr };
    }

    // D9 and "a plugin with a sidechain bus": the slot must be keyable at all.
    QString why;
    if( !keyed->sidechainRoutable( slot_, &why ) ) {
        qWarning().noquote() << name() << ": refused, cannot key" << keyed->getSName()
                             << "slot" << slot_ << "--" << why;
        return { false, nullptr };
    }

    // BY ROLE, as add-send and add-return-track refuse the master: its output
    // IS the sum that already contains every lane, and the edge that closes
    // that cycle is the mixer's own wiring, which the routing graph lacks.
    if( source->systemRole() == SSystemRole::Master ) {
        qWarning().noquote()
            << name() << ": refused, the MASTER lane cannot key a sidechain --"
            << "its output is the sum that already contains" << keyed->getSName();
        return { false, nullptr };
    }
    if( source->systemRole() == SSystemRole::Conductor ) {
        qWarning().noquote()
            << name() << ": refused, the conductor lane carries no audio to key"
            << keyed->getSName() << "from";
        return { false, nullptr };
    }
    if( source == keyed ) {
        qWarning().noquote()
            << name() << ": refused," << keyed->getSName()
            << "cannot key its own slot (a self edge)";
        return { false, nullptr };
    }

    // THE CYCLE WALK over the union of edge kinds (D7), the same sroute::Graph
    // the pass breaks cycles with. The slot's CURRENT key edge (if any) enters
    // `keyed` and so cannot lie on a path from `keyed` to `source`: asking the
    // graph with it still in place answers what the graph without it would.
    const sroute::Graph g = mixer->routingGraph();
    if( g.wouldCloseCycle( static_cast<sroute::Node>( static_cast<SObject *>( source ) ),
                           static_cast<sroute::Node>( static_cast<SObject *>( keyed ) ) ) ) {
        qWarning().noquote()
            << name() << ": refused, keying" << keyed->getSName() << "from"
            << source->getSName() << "would close a routing CYCLE --"
            << keyed->getSName() << "already reaches" << source->getSName()
            << "(proposal 55 D7)";
        return { false, nullptr };
    }

    SAction *inverse = new SRestorePluginSidechainAction(
        splacements::specOf( project, keyed ), slot_,
        keyed->pluginSlotHasSidechain( slot_ ),
        keyed->pluginSlotSidechainSource( slot_ ),
        keyed->pluginSlotSidechainPre( slot_ ) );
    keyed->setPluginSlotSidechain( slot_, source, tap == QLatin1String( "pre" ) );
    return { true, inverse };
}

void SSetPluginSidechainAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slot", slot_ );
    elem.setAttribute( "source", source_ );
    if( !tap_.isEmpty() ) elem.setAttribute( "tap", tap_ );
}

bool SSetPluginSidechainAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    slot_      = elem.attribute( "slot", "0" ).toInt();
    source_    = elem.attribute( "source" );
    tap_       = elem.attribute( "tap" );
    return !trackPath_.isEmpty() && !source_.isEmpty();
}

static const bool s_reg_set_plugin_sidechain = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-plugin-sidechain" ),
        []{ return new SSetPluginSidechainAction; } ),
    true );

// --- clear-plugin-sidechain -------------------------------------------------

SClearPluginSidechainAction::SClearPluginSidechainAction( const QString &trackPath,
                                                          int slot )
    : trackPath_( trackPath ), slot_( slot )
{
}

SApplyResult SClearPluginSidechainAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    STrack *keyed = resolveLane( project, name(), trackPath_, pathRoot_, nullptr,
                                 "keyed track" );
    if( !keyed ) return { false, nullptr };
    if( slot_ < 0 || slot_ >= keyed->pluginSlotCount() ) {
        qWarning().noquote() << name() << ": refused, there is no plugin in slot"
                             << slot_ << "of" << keyed->getSName();
        return { false, nullptr };
    }
    if( !keyed->pluginSlotHasSidechain( slot_ ) ) {
        qWarning().noquote() << name() << ": refused, slot" << slot_ << "of"
                             << keyed->getSName() << "is not keyed";
        return { false, nullptr };
    }
    SAction *inverse = new SRestorePluginSidechainAction(
        splacements::specOf( project, keyed ), slot_, true,
        keyed->pluginSlotSidechainSource( slot_ ),
        keyed->pluginSlotSidechainPre( slot_ ) );
    keyed->clearPluginSlotSidechain( slot_ );
    return { true, inverse };
}

void SClearPluginSidechainAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slot", slot_ );
}

bool SClearPluginSidechainAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    slot_      = elem.attribute( "slot", "0" ).toInt();
    return !trackPath_.isEmpty();
}

static const bool s_reg_clear_plugin_sidechain = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "clear-plugin-sidechain" ),
        []{ return new SClearPluginSidechainAction; } ),
    true );

// --- the inverse ------------------------------------------------------------

SRestorePluginSidechainAction::SRestorePluginSidechainAction( const QString &trackSpec,
                                                              int slot, bool set,
                                                              SObject *source,
                                                              bool preFader )
    : trackSpec_( trackSpec ), slot_( slot ), set_( set ), source_( source ),
      pre_( preFader )
{
}

SApplyResult SRestorePluginSidechainAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    QString root;
    STrack *keyed = dynamic_cast<STrack *>(
        splacements::laneBySpec( project, trackSpec_, root ) );
    if( !keyed || slot_ < 0 || slot_ >= keyed->pluginSlotCount() )
        return { false, nullptr };
    SAction *inverse = new SRestorePluginSidechainAction(
        trackSpec_, slot_, keyed->pluginSlotHasSidechain( slot_ ),
        keyed->pluginSlotSidechainSource( slot_ ),
        keyed->pluginSlotSidechainPre( slot_ ) );
    keyed->setPluginSlotSidechain( slot_, source_.data(), pre_, set_ );
    return { true, inverse };
}

void SRestorePluginSidechainAction::writeXml( QDomElement &elem ) const
{
    // Never serialized standalone (built live as an inverse); recorded for
    // completeness, as restore-return-source does.
    elem.setAttribute( "trackPath", trackSpec_ );
    elem.setAttribute( "slot", slot_ );
    elem.setAttribute( "keyed", set_ ? 1 : 0 );
    elem.setAttribute( "tap", pre_ ? "pre" : "post" );
}
