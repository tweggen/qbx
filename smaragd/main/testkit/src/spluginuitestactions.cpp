#include "app/testkit/spluginuitestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/pluginui/splugineffectstrip.h"
#include "tw/plugins/twpluginslotproc.h"

#include <QDebug>
#include <QDomElement>
#include <memory>

namespace {

STrack *trackAt( SProject *project, int index )
{
    if( !project ) return nullptr;
    SStdMixer *mixer = dynamic_cast<SStdMixer *>( project->getRootComponent() );
    if( !mixer ) return nullptr;
    if( index < 0 || index >= mixer->getNTracks() ) return nullptr;
    SLink *link = mixer->getTrackAt( index );
    return link ? dynamic_cast<STrack *>( &link->getSObject() ) : nullptr;
}

// A strip built for the assertion and thrown away. Parentless and never shown,
// so no native window is created; its constructor runs the real rebuildUI(),
// which is the thing under test.
std::unique_ptr<SPluginEffectStrip> makeStrip( SProject *project, int trackIndex )
{
    STrack *track = trackAt( project, trackIndex );
    if( !track ) return nullptr;
    return std::make_unique<SPluginEffectStrip>( track, nullptr );
}

STrack *trackAtPath( SProject *project, const QString &trackPath )
{
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( trackPath ) );
    return dynamic_cast<STrack *>( lane );
}

const char *slotModeName( audio::twPluginSlotMode m )
{
    switch( m ) {
        case audio::twPluginSlotMode::Transparent: return "Transparent";
        case audio::twPluginSlotMode::Direct:      return "Direct";
        case audio::twPluginSlotMode::DualMono:    return "DualMono";
        case audio::twPluginSlotMode::MonoFold:    return "MonoFold";
        case audio::twPluginSlotMode::DirectGen:   return "DirectGen";
        case audio::twPluginSlotMode::MonoSpread:  return "MonoSpread";
        case audio::twPluginSlotMode::GenFold:     return "GenFold";
        case audio::twPluginSlotMode::WideGen:     return "WideGen";
    }
    return "Transparent";
}

const char *slotStateName( audio::twPluginSlotState st )
{
    switch( st ) {
        case audio::twPluginSlotState::Active:      return "Active";
        case audio::twPluginSlotState::Missing:     return "Missing";
        case audio::twPluginSlotState::Unsupported: return "Unsupported";
    }
    return "Active";
}

// Path-addressed variant: the only way to reach a track nested in a folder.
std::unique_ptr<SPluginEffectStrip> makeStripAt( SProject *project,
                                                 const QString &trackPath )
{
    SObject *root = splacements::rootContainer( project );
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( trackPath ) );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) return nullptr;
    return std::make_unique<SPluginEffectStrip>( track, nullptr );
}

}  // namespace

SApplyResult SAssertPluginStripAction::apply( SProject *project )
{
    auto strip = trackPath_.isEmpty() ? makeStrip( project, trackIndex_ )
                                      : makeStripAt( project, trackPath_ );
    if( !strip ) {
        qWarning() << "assert-plugin-strip: no track"
                   << ( trackPath_.isEmpty() ? QString::number( trackIndex_ )
                                             : trackPath_ );
        return { false, nullptr };
    }

    if( slotCount_ >= 0 && strip->slotRowCount() != slotCount_ ) {
        qWarning() << "assert-plugin-strip FAILED: strip rendered"
                   << strip->slotRowCount() << "rows, expected" << slotCount_;
        return { false, nullptr };
    }

    if( slotIndex_ >= 0 && ( !contains_.isEmpty() || !absent_.isEmpty() ) ) {
        const QString desc = strip->describeSlot( slotIndex_ );
        if( desc.isEmpty() ) {
            qWarning() << "assert-plugin-strip FAILED: no row" << slotIndex_;
            return { false, nullptr };
        }
        if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
            qWarning() << "assert-plugin-strip FAILED: row" << slotIndex_
                       << "does not contain" << contains_ << "; it is" << desc;
            return { false, nullptr };
        }
        if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
            qWarning() << "assert-plugin-strip FAILED: row" << slotIndex_
                       << "unexpectedly contains" << absent_ << "; it is" << desc;
            return { false, nullptr };
        }
    }

    return { true, nullptr };   // an assertion has nothing to undo
}

void SAssertPluginStripAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "trackIndex", trackIndex_ );
    elem.setAttribute( "slotCount", slotCount_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
}

bool SAssertPluginStripAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_  = elem.attribute( "trackPath" );
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    slotCount_  = elem.attribute( "slotCount", "-1" ).toInt();
    slotIndex_  = elem.attribute( "slotIndex", "-1" ).toInt();
    contains_   = elem.attribute( "contains" );
    absent_     = elem.attribute( "absent" );
    return true;
}

SApplyResult SPluginEditorSetParamAction::apply( SProject *project )
{
    auto strip = trackPath_.isEmpty() ? makeStrip( project, trackIndex_ )
                                      : makeStripAt( project, trackPath_ );
    if( !strip ) {
        qWarning() << "plugin-editor-set-param: no track"
                   << ( trackPath_.isEmpty() ? QString::number( trackIndex_ )
                                             : trackPath_ );
        return { false, nullptr };
    }

    // editorSetParam() opens (unshown) the very editor a double-click opens and
    // moves the slider, which submits a set-plugin-param action of its own. That
    // nested action is what lands on the undo stack — this verb is not undoable
    // itself, so `<undo count="1"/>` after it undoes the parameter edit.
    if( !strip->editorSetParam( slotIndex_, paramId_, value_ ) ) {
        qWarning() << "plugin-editor-set-param FAILED: slot" << slotIndex_
                   << "has no editable parameter id" << paramId_;
        return { false, nullptr };
    }

    if( !expectValueText_.isEmpty() ) {
        const QString shown = strip->editorValueText( slotIndex_, expectValueRow_ );
        if( shown != expectValueText_ ) {
            qWarning() << "plugin-editor-set-param FAILED: value label row"
                       << expectValueRow_ << "shows" << shown << "expected"
                       << expectValueText_;
            return { false, nullptr };
        }
    }

    return { true, nullptr };
}

void SPluginEditorSetParamAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "trackIndex", trackIndex_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "paramId", (qulonglong) paramId_ );
    elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
    if( !expectValueText_.isEmpty() ) {
        elem.setAttribute( "expectValueText", expectValueText_ );
        elem.setAttribute( "expectValueRow", expectValueRow_ );
    }
}

bool SPluginEditorSetParamAction::readXml( const QDomElement &elem,
                                          int /*version*/ )
{
    trackPath_  = elem.attribute( "trackPath" );
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    slotIndex_  = elem.attribute( "slotIndex", "0" ).toInt();
    paramId_    = (std::uint32_t) elem.attribute( "paramId", "0" ).toUInt();
    value_      = elem.attribute( "value", "0" ).toDouble();
    expectValueText_ = elem.attribute( "expectValueText" );
    expectValueRow_  = elem.attribute( "expectValueRow", "0" ).toInt();
    return true;
}

// --- assert-instrument-slot (proposal 37 P3b) ------------------------------

SApplyResult SAssertInstrumentSlotAction::apply( SProject *project )
{
    STrack *track = trackPath_.isEmpty() ? trackAt( project, trackIndex_ )
                                         : trackAtPath( project, trackPath_ );
    if( !track ) {
        qWarning() << "assert-instrument-slot: no track"
                   << ( trackPath_.isEmpty() ? QString::number( trackIndex_ )
                                             : trackPath_ );
        return { false, nullptr };
    }

    SPluginSlot *slot = track->instrumentSlot();
    if( present_ == 0 ) {
        if( slot ) {
            qWarning() << "assert-instrument-slot FAILED: expected NO instrument,"
                       << "found" << QString::fromStdString( slot->getDescriptor().uid );
            return { false, nullptr };
        }
        return { true, nullptr };
    }
    if( !slot ) {
        qWarning() << "assert-instrument-slot FAILED: slot 0 of the track is not"
                      " an instrument (or the chain is empty)";
        return { false, nullptr };
    }

    const audio::twPluginDescriptor &d = slot->getDescriptor();
    if( !uid_.isEmpty() && QString::fromStdString( d.uid ) != uid_ ) {
        qWarning() << "assert-instrument-slot FAILED: uid is"
                   << QString::fromStdString( d.uid ) << "expected" << uid_;
        return { false, nullptr };
    }
    if( !format_.isEmpty() && QString::fromStdString( d.format ) != format_ ) {
        qWarning() << "assert-instrument-slot FAILED: format is"
                   << QString::fromStdString( d.format ) << "expected" << format_;
        return { false, nullptr };
    }
    if( !mode_.isEmpty() ) {
        const QString have = QLatin1String( slotModeName( slot->getSlotMode() ) );
        if( have != mode_ ) {
            qWarning() << "assert-instrument-slot FAILED: mode is" << have
                       << "expected" << mode_;
            return { false, nullptr };
        }
    }
    if( !state_.isEmpty() ) {
        const QString have = QLatin1String( slotStateName( slot->getSlotState() ) );
        if( have != state_ ) {
            qWarning() << "assert-instrument-slot FAILED: state is" << have
                       << "expected" << state_;
            return { false, nullptr };
        }
    }
    if( minTailFrames_ >= 0 || maxTailFrames_ >= 0 ) {
        const long long tail = (long long) slot->tailFrames();
        if( minTailFrames_ >= 0 && tail < minTailFrames_ ) {
            qWarning() << "assert-instrument-slot FAILED: tailFrames" << tail
                       << "<" << minTailFrames_;
            return { false, nullptr };
        }
        if( maxTailFrames_ >= 0 && tail > maxTailFrames_ ) {
            qWarning() << "assert-instrument-slot FAILED: tailFrames" << tail
                       << ">" << maxTailFrames_;
            return { false, nullptr };
        }
    }
    if( hasFeed_ >= 0 ) {
        const std::shared_ptr<audio::twPluginSlotProcessor> &proc = slot->getProcessor();
        const bool have = proc && proc->hasEventSource();
        if( have != ( hasFeed_ != 0 ) ) {
            qWarning() << "assert-instrument-slot FAILED: hasFeed is" << have
                       << "expected" << ( hasFeed_ != 0 );
            return { false, nullptr };
        }
    }
    return { true, nullptr };   // an assertion has nothing to undo
}

void SAssertInstrumentSlotAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "trackIndex", trackIndex_ );
    elem.setAttribute( "present", present_ );
    if( !uid_.isEmpty() )    elem.setAttribute( "uid", uid_ );
    if( !format_.isEmpty() ) elem.setAttribute( "format", format_ );
    if( !mode_.isEmpty() )   elem.setAttribute( "mode", mode_ );
    if( !state_.isEmpty() )  elem.setAttribute( "state", state_ );
    if( minTailFrames_ >= 0 )
        elem.setAttribute( "minTailFrames", QString::number( minTailFrames_ ) );
    if( maxTailFrames_ >= 0 )
        elem.setAttribute( "maxTailFrames", QString::number( maxTailFrames_ ) );
    if( hasFeed_ >= 0 ) elem.setAttribute( "hasFeed", hasFeed_ );
}

bool SAssertInstrumentSlotAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_  = elem.attribute( "trackPath" );
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    present_    = elem.attribute( "present", "1" ).toInt();
    uid_        = elem.attribute( "uid" );
    format_     = elem.attribute( "format" );
    mode_       = elem.attribute( "mode" );
    state_      = elem.attribute( "state" );
    minTailFrames_ = elem.attribute( "minTailFrames", "-1" ).toLongLong();
    maxTailFrames_ = elem.attribute( "maxTailFrames", "-1" ).toLongLong();
    hasFeed_    = elem.attribute( "hasFeed", "-1" ).toInt();
    return true;
}

static const bool s_reg_assertinstrumentslot =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-instrument-slot" ),
          [] { return new SAssertInstrumentSlotAction; } ),
      true );

static const bool s_reg_assertpluginstrip =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-plugin-strip" ),
          [] { return new SAssertPluginStripAction; } ),
      true );

static const bool s_reg_plugineditorsetparam =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "plugin-editor-set-param" ),
          [] { return new SPluginEditorSetParamAction; } ),
      true );
