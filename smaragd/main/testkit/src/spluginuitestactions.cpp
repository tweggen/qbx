#include "app/testkit/spluginuitestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/spluginchain.h"
// The slot's own header is the sanctioned route to the engine's
// twPluginSlotMode / twPluginSlotState / twPluginSlotProcessor: testkit has no
// edge to tw/plugins (check_layering.py), and it does not need one — every
// question below is asked of the APP model, which quotes those types in its own
// public API. Including tw/plugins/… directly here is a layering violation even
// though it would compile.
#include "app/objects/track/spluginslot.h"
#include "app/pluginui/splugineffectstrip.h"
#include "app/pluginui/spluginnativeeditor.h"

#include <QCoreApplication>
#include <QEvent>

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

STrack *trackAtPath( SProject *project, const QString &pathRoot_,
                     const QString &trackPath )
{
    // The path text may carry its own root qualifier ("Drums:0"); an
    // explicit pathRoot_ is the fallback for the bare spelling.
    const strackpath::QualifiedPath q_ = strackpath::parseQualified( trackPath );
    SObject *root = splacements::rootNamed(
        project, q_.root.isEmpty() ? pathRoot_ : q_.root );
    SObject *lane = splacements::laneAt( root, q_.idx );
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
                                                 const QString &pathRoot_,
                                                 const QString &trackPath )
{
    // The path text may carry its own root qualifier ("Drums:0"); an
    // explicit pathRoot_ is the fallback for the bare spelling.
    const strackpath::QualifiedPath q_ = strackpath::parseQualified( trackPath );
    SObject *root = splacements::rootNamed(
        project, q_.root.isEmpty() ? pathRoot_ : q_.root );
    SObject *lane = splacements::laneAt( root, q_.idx );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) return nullptr;
    return std::make_unique<SPluginEffectStrip>( track, nullptr );
}

}  // namespace

SApplyResult SAssertPluginStripAction::apply( SProject *project )
{
    auto strip = trackPath_.isEmpty() ? makeStrip( project, trackIndex_ )
                                      : makeStripAt( project, pathRoot_, trackPath_ );
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
                                      : makeStripAt( project, pathRoot_, trackPath_ );
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
                                         : trackAtPath( project, pathRoot_, trackPath_ );
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

SApplyResult SAssertPluginEditorKindAction::apply( SProject *project )
{
    auto strip = trackPath_.isEmpty() ? makeStrip( project, trackIndex_ )
                                      : makeStripAt( project, pathRoot_, trackPath_ );
    if( !strip ) {
        qWarning() << "assert-plugin-editor-kind: no track"
                   << ( trackPath_.isEmpty() ? QString::number( trackIndex_ )
                                             : trackPath_ );
        return { false, nullptr };
    }

    if( expect_ != QLatin1String( "native" ) &&
        expect_ != QLatin1String( "generic" ) &&
        expect_ != QLatin1String( "none" ) ) {
        qWarning() << "assert-plugin-editor-kind: expect must be one of "
                      "native|generic|none, got" << expect_;
        return { false, nullptr };
    }

    const QString kind = strip->editorKindFor( slotIndex_ );
    if( kind != expect_ ) {
        qWarning() << "assert-plugin-editor-kind FAILED: slot" << slotIndex_
                   << "would open" << kind << "expected" << expect_;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertPluginEditorKindAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "trackIndex", trackIndex_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "expect", expect_ );
}

bool SAssertPluginEditorKindAction::readXml( const QDomElement &elem, int )
{
    trackPath_  = elem.attribute( "trackPath" );
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    slotIndex_  = elem.attribute( "slotIndex", "0" ).toInt();
    expect_     = elem.attribute( "expect" );
    // Deliberately NOT rejected here when empty. readXml must accept whatever
    // writeXml produced, including a default-constructed action -- that round
    // trip is itself a gate (action_roundtrip_test). A missing or misspelt
    // `expect` is a SCRIPT error, so it is caught in apply() where it can be
    // reported against the three legal values instead of as a parse failure.
    return true;
}

SApplyResult SPluginNativeEditorAction::apply( SProject *project )
{
    STrack *track = trackPath_.isEmpty() ? trackAt( project, trackIndex_ )
                                         : trackAtPath( project, pathRoot_, trackPath_ );
    if( !track ) {
        qWarning() << "plugin-native-editor: no track"
                   << ( trackPath_.isEmpty() ? QString::number( trackIndex_ )
                                             : trackPath_ );
        return { false, nullptr };
    }
    SPluginChain *chain = track->getPluginChain();
    SPluginSlot  *slot  = chain ? chain->getSlotAt( slotIndex_ ) : nullptr;
    if( !slot ) {
        qWarning() << "plugin-native-editor: no slot" << slotIndex_;
        return { false, nullptr };
    }

    if( action_ == QLatin1String( "open" ) ) {
        // showWindow = false: see the header. The attach is real; only the
        // mapping of our container onto the screen is skipped.
        SPluginNativeEditor::openFor( track, slot, nullptr, /*showWindow=*/false );
    } else if( action_ == QLatin1String( "assert" ) ) {
        // Opens and closes nothing; the expectOpen check below is the whole
        // verb. It exists so a case can say "and NOTHING opened" about a step
        // that was supposed to be inert.
    } else if( action_ == QLatin1String( "restore" ) ) {
        // Proposal 33 D2. Drives the real post-load walk, which is a NO-OP
        // under --test-case: a qxa run uses the real platform plugin, so a
        // restored editor would put a plugin window on the developer's screen
        // mid-suite. `expectOpen="0"` is what asserts the guard held.
        SPluginNativeEditor::restoreOpenEditors( project, nullptr );
    } else if( action_ == QLatin1String( "close" ) ) {
        SPluginNativeEditor::closeFor( slot );
        // The dialogs are WA_DeleteOnClose, so close() only POSTS the deletion.
        // Draining exactly that event here is what makes the check below mean
        // "it is gone" rather than "it has been asked to go".
        QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    } else {
        qWarning() << "plugin-native-editor: action must be "
                      "open|close|assert|restore, got" << action_;
        return { false, nullptr };
    }

    const bool open = SPluginNativeEditor::isOpenFor( slot );
    if( open != ( expectOpen_ != 0 ) ) {
        qWarning() << "plugin-native-editor FAILED: after" << action_
                   << "the slot's native editor is" << ( open ? "open" : "closed" )
                   << "expected" << ( expectOpen_ ? "open" : "closed" );
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SPluginNativeEditorAction::writeXml( QDomElement &elem ) const
{
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "trackIndex", trackIndex_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
    elem.setAttribute( "action", action_ );
    elem.setAttribute( "expectOpen", expectOpen_ );
}

bool SPluginNativeEditorAction::readXml( const QDomElement &elem, int )
{
    trackPath_  = elem.attribute( "trackPath" );
    trackIndex_ = elem.attribute( "trackIndex", "0" ).toInt();
    slotIndex_  = elem.attribute( "slotIndex", "0" ).toInt();
    action_     = elem.attribute( "action", "open" );
    expectOpen_ = elem.attribute( "expectOpen", "1" ).toInt();
    return true;
}

static const bool s_reg_pluginnativeeditor =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "plugin-native-editor" ),
          [] { return new SPluginNativeEditorAction; } ),
      true );

static const bool s_reg_assertplugineditorkind =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-plugin-editor-kind" ),
          [] { return new SAssertPluginEditorKindAction; } ),
      true );

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
