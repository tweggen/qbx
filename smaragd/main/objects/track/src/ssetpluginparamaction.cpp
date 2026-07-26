#include "app/objects/track/ssetpluginparamaction.h"

#include "app/actions/sactionregistry.h"
#include "app/objects/track/spluginactionsupport.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twpluginslotproc.h"

#include <QDebug>
#include <QDomElement>

SSetPluginParamAction::SSetPluginParamAction( const QString &trackPath,
                                             int slotIndex,
                                             std::uint32_t paramId, double value )
    : trackPath_( trackPath ), slotIndex_( slotIndex ), paramId_( paramId ),
      value_( value )
{
}

SApplyResult SSetPluginParamAction::apply( SProject *project )
{
    SPluginSlot *slot = spluginaction::slotFor( project, trackPath_, slotIndex_ );
    if( !slot ) {
        qWarning() << "set-plugin-param: no slot" << slotIndex_
                   << "on track" << trackPath_;
        return { false, nullptr };
    }

    // Refuse on a slot that is not Active. A Missing/Unsupported slot runs
    // createNullPlugin()'s placeholder, which has NO parameters — the edit would
    // be silently dropped, and worse, the authority on a Missing slot's settings
    // is the stored state chunk (SPluginSlot::saveState() never reads a
    // non-Active slot), so a "successful" edit here would be a lie the user
    // could not see until the plugin came back.
    if( slot->getSlotState() != audio::twPluginSlotState::Active ) {
        qWarning() << "set-plugin-param: slot" << slotIndex_ << "on track"
                   << trackPath_ << "is not active; refusing the edit";
        return { false, nullptr };
    }

    const std::shared_ptr<audio::twPluginSlotProcessor> &proc = slot->getProcessor();
    audio::twPlugin *p0 = proc ? proc->plugin() : nullptr;
    if( !p0 ) {
        qWarning() << "set-plugin-param: slot" << slotIndex_ << "has no instance";
        return { false, nullptr };
    }

    // Validate the id against the plugin's own list and clamp to its declared
    // range. Without this, a typo'd id would be accepted by every backend's
    // setParam() as a no-op and the action would report success — which would
    // make a headless test that asserts a level change fail for a reason nowhere
    // near the truth.
    bool found = false;
    audio::twPluginParamInfo info{};
    const std::size_t nParams = p0->paramCount();
    for( std::size_t i = 0; i < nParams; ++i ) {
        const audio::twPluginParamInfo cand = p0->paramInfo( i );
        if( cand.id == paramId_ ) {
            info  = cand;
            found = true;
            break;
        }
    }
    if( !found ) {
        qWarning() << "set-plugin-param: plugin has no parameter id" << paramId_;
        return { false, nullptr };
    }

    double v = value_;
    if( info.maxValue > info.minValue ) {
        if( v < info.minValue ) v = info.minValue;
        if( v > info.maxValue ) v = info.maxValue;
    }

    const double oldValue = p0->getParam( paramId_ );

    // EVERY instance: the dual-mono mapping (a 1->1 plugin on N buses) runs N
    // independent plugins, and they must stay in lockstep or the buses drift
    // apart on the first parameter edit.
    for( audio::twPlugin *p : proc->plugins() ) {
        if( p ) p->setParam( paramId_, v );
    }

    // The load-bearing second call: setParam() alone is inaudible. This bumps
    // the processor's param epoch (moving its all-bus cache key and staling
    // every tap's frozen pages) and invalidates the render path above the slot.
    slot->notifyPluginEdited();

    return { true, new SSetPluginParamAction( trackPath_, slotIndex_, paramId_,
                                              oldValue ) };
}

void SSetPluginParamAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "paramId", (qulonglong) paramId_ );
    // 17 significant digits: a double round-trips exactly, so an undo entry
    // replayed from a saved action script lands on the same sample values.
    elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
}

bool SSetPluginParamAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    paramId_   = (std::uint32_t) elem.attribute( "paramId", "0" ).toUInt();
    value_     = elem.attribute( "value", "0" ).toDouble();
    return true;
}

QString SSetPluginParamAction::mergeKey() const
{
    // Same slot AND same parameter -> same key, so one slider drag collapses to
    // one undo entry. A different parameter must NOT merge: the inverse only
    // carries one (paramId, oldValue) pair.
    return QStringLiteral( "set-plugin-param:%1:%2:%3" )
        .arg( trackPath_ )
        .arg( slotIndex_ )
        .arg( paramId_ );
}

bool SSetPluginParamAction::mergeWith( const SAction *later )
{
    const SSetPluginParamAction *o =
        dynamic_cast<const SSetPluginParamAction *>( later );
    if( !o || o->slotIndex_ != slotIndex_ || o->paramId_ != paramId_ ||
        o->trackPath_ != trackPath_ ) {
        return false;
    }
    // Absorb the newer target value; our own value stays the baseline the
    // already-captured inverse was measured against.
    value_ = o->value_;
    return true;
}

static const bool s_reg_setpluginparam =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "set-plugin-param" ),
          [] { return new SSetPluginParamAction; } ),
      true );
