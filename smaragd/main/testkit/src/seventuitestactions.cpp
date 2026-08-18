#include "app/testkit/seventuitestactions.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QDomElement>
#include <QThread>

#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "tw/devices/midi_out_scheduler.h"

namespace {

// The docks live under the main window, and testkit may not include
// app/eventui — so reach them through the shell, the same route drag-clip-edge
// and assert-meter take (testkit CONTRACT inv. 5).
SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    return nullptr;
}

}  // namespace

// ------------------------------------------------------------- virtual-key

SApplyResult SVirtualKeyAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "virtual-key: no main window";
        return { false, nullptr };
    }

    // --- PLAY the port (proposal 21 L2) -----------------------------------
    if( release_ ) {
        if( !win->virtualKeyRelease( key_ ) ) {
            qWarning() << "virtual-key: release failed for key" << key_;
            return { false, nullptr };
        }
        return { true, nullptr };
    }
    if( hold_ ) {
        if( !win->virtualKeyHold( key_, velocity_ ) ) {
            qWarning() << "virtual-key: hold failed for key" << key_;
            return { false, nullptr };
        }
        if( durationMs_ > 0 ) {
            // Paced on the ONE steady clock, pumping: a bare sleep would stop
            // the locator, the meters and the pumps the case is measuring.
            const qint64 due = audio::MidiOutScheduler::hostNowNs()
                               + (qint64) durationMs_ * 1000000LL;
            while( audio::MidiOutScheduler::hostNowNs() < due ) {
                QCoreApplication::processEvents();
                QThread::msleep( 1 );
            }
            win->virtualKeyRelease( key_ );
        }
        return { true, nullptr };
    }

    if( !win->virtualKey( key_, velocity_, durationTicks_ ) ) {
        qWarning() << "virtual-key: no event clip to write into (key" << key_
                   << ")";
        return { false, nullptr };
    }
    // The keyboard submits its own add-note; undoing THAT is what reverses this
    // gesture, exactly as for drag-clip-edge.
    return { true, nullptr };
}

void SVirtualKeyAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "key", key_ );
    elem.setAttribute( "velocity", QString::number( velocity_ ) );
    elem.setAttribute( "durationTicks", QString::number( durationTicks_ ) );
    if( hold_ )    elem.setAttribute( "hold", "1" );
    if( release_ ) elem.setAttribute( "release", "1" );
    if( durationMs_ > 0 ) elem.setAttribute( "durationMs", durationMs_ );
}

bool SVirtualKeyAction::readXml( const QDomElement &elem, int )
{
    key_ = elem.attribute( "key", "60" ).toInt();
    velocity_ = elem.attribute( "velocity", "100" ).toDouble();
    durationTicks_ = elem.attribute( "durationTicks", "960" ).toLongLong();
    hold_    = elem.attribute( "hold", "0" ).toInt() != 0;
    release_ = elem.attribute( "release", "0" ).toInt() != 0;
    durationMs_ = elem.attribute( "durationMs", "0" ).toInt();
    // key=-1 means "every held key" and is legal for RELEASE only.
    if( release_ && key_ == -1 ) return true;
    if( key_ < 0 || key_ > 127 ) {
        qWarning() << "virtual-key: key out of range:" << key_;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- drag-note

QStringList SDragNoteAction::knownAttributes() const
{
    return { QStringLiteral( "clip" ),    QStringLiteral( "tick" ),
             QStringLiteral( "key" ),     QStringLiteral( "channel" ),
             QStringLiteral( "toTick" ),  QStringLiteral( "toKey" ),
             QStringLiteral( "edge" ),    QStringLiteral( "lane" ),
             QStringLiteral( "toValue" ) };
}

SApplyResult SDragNoteAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "drag-note: no main window";
        return { false, nullptr };
    }
    if( !win->dragNote( clip_, tick_, key_, channel_, toTick_, toKey_, edge_,
                        lane_, toValue_ ) ) {
        qWarning() << "drag-note: no note at tick" << tick_ << "key" << key_
                   << "in clip" << clip_;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SDragNoteAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clip_ );
    elem.setAttribute( "tick", QString::number( tick_ ) );
    elem.setAttribute( "key", key_ );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "toTick", QString::number( toTick_ ) );
    elem.setAttribute( "toKey", toKey_ );
    elem.setAttribute( "edge", edge_ );
    elem.setAttribute( "lane", lane_ );
    elem.setAttribute( "toValue", QString::number( toValue_ ) );
}

bool SDragNoteAction::readXml( const QDomElement &elem, int )
{
    clip_    = elem.attribute( "clip" );
    tick_    = elem.attribute( "tick", "0" ).toLongLong();
    key_     = elem.attribute( "key", "60" ).toInt();
    channel_ = elem.attribute( "channel", "-1" ).toInt();
    toTick_  = elem.attribute( "toTick", "0" ).toLongLong();
    toKey_   = elem.attribute( "toKey", "-1" ).toInt();
    edge_    = elem.attribute( "edge" );
    lane_    = elem.attribute( "lane", "notes" );
    toValue_ = elem.attribute( "toValue", "-1" ).toDouble();

    if( !edge_.isEmpty() && edge_ != QLatin1String( "end" ) ) {
        qWarning() << "drag-note: unknown edge:" << edge_;
        return false;
    }
    if( lane_ != QLatin1String( "notes" )
        && lane_ != QLatin1String( "velocity" ) ) {
        qWarning() << "drag-note: unknown lane:" << lane_;
        return false;
    }
    if( lane_ == QLatin1String( "velocity" ) && toValue_ < 0.0 ) {
        qWarning() << "drag-note: lane=velocity needs toValue";
        return false;
    }
    if( lane_ == QLatin1String( "notes" ) && edge_.isEmpty() && toKey_ < 0 ) {
        // A move needs a destination key; a resize does not.
        toKey_ = key_;
    }
    return true;
}

// ------------------------------------------------------- assert-event-editor

QStringList SAssertEventEditorAction::knownAttributes() const
{
    return { QStringLiteral( "clip" ),     QStringLiteral( "kind" ),
             QStringLiteral( "contains" ), QStringLiteral( "absent" ),
             QStringLiteral( "grabPng" ),  QStringLiteral( "grabWidth" ),
             QStringLiteral( "grabHeight" ) };
}

SApplyResult SAssertEventEditorAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-event-editor: no main window";
        return { false, nullptr };
    }

    const QString desc = win->describeEventEditor( clip_, kind_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-event-editor FAILED: the dock described nothing"
                   << "(kind" << kind_ << ")";
        return { false, nullptr };
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-event-editor FAILED: editor reports" << desc
                   << "which lacks" << contains_;
        return { false, nullptr };
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        qWarning() << "assert-event-editor FAILED: editor reports" << desc
                   << "which unexpectedly contains" << absent_;
        return { false, nullptr };
    }

    if( !grabPng_.isEmpty() ) {
        if( grabPng_.contains( '/' ) || grabPng_.contains( '\\' )
            || grabPng_.contains( ".." ) ) {
            qWarning() << "assert-event-editor: grabPng contains path"
                          " separators:" << grabPng_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-event-editor FAILED: no usable test output"
                          " directory";
            return { false, nullptr };
        }
        const QString out = QDir( app.testOutputDir() ).filePath( grabPng_ );
        if( !win->grabEventEditor( out, grabWidth_, grabHeight_ ) ) {
            qWarning() << "assert-event-editor FAILED: could not paint the"
                          " editor into" << out;
            return { false, nullptr };
        }
    }
    return { true, nullptr };   // an assertion has nothing to undo
}

void SAssertEventEditorAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clip_ );
    elem.setAttribute( "kind", kind_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
    elem.setAttribute( "grabPng", grabPng_ );
    elem.setAttribute( "grabWidth", grabWidth_ );
    elem.setAttribute( "grabHeight", grabHeight_ );
}

bool SAssertEventEditorAction::readXml( const QDomElement &elem, int )
{
    clip_       = elem.attribute( "clip" );
    kind_       = elem.attribute( "kind", "pianoroll" );
    contains_   = elem.attribute( "contains" );
    absent_     = elem.attribute( "absent" );
    grabPng_    = elem.attribute( "grabPng" );
    grabWidth_  = elem.attribute( "grabWidth", "720" ).toInt();
    grabHeight_ = elem.attribute( "grabHeight", "360" ).toInt();
    return true;
}

// -------------------------------------------------------- assert-track-head

SApplyResult SAssertTrackHeadAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "assert-track-head: no main window";
        return { false, nullptr };
    }
    const QString desc = win->describeTrackHead( trackPath_, headHeight_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-track-head FAILED: no head for track"
                   << trackPath_;
        return { false, nullptr };
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-track-head FAILED: head at height" << headHeight_
                   << "reports" << desc << "which lacks" << contains_;
        return { false, nullptr };
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        qWarning() << "assert-track-head FAILED: head at height" << headHeight_
                   << "reports" << desc << "which unexpectedly contains"
                   << absent_;
        return { false, nullptr };
    }
    // Coverage of the head's paint path, including the automation button's
    // per-mode colour (proposal 37 P6). Never an oracle.
    if( !grabPng_.isEmpty() ) {
        if( grabPng_.contains( '/' ) || grabPng_.contains( '\\' )
            || grabPng_.contains( ".." ) ) {
            qWarning() << "assert-track-head: grabPng contains path separators:"
                       << grabPng_;
            return { false, nullptr };
        }
        SApplication &app = SApplication::app();
        if( app.testOutputDir().isEmpty() || !app.ensureOutputDirExists() ) {
            qWarning() << "assert-track-head FAILED: no usable test output"
                          " directory";
            return { false, nullptr };
        }
        const QString out = QDir( app.testOutputDir() ).filePath( grabPng_ );
        if( !win->grabTrackHead( out, trackPath_, headHeight_, grabWidth_,
                                 grabHeight_ ) ) {
            qWarning() << "assert-track-head FAILED: could not grab the head"
                          " into" << out;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SAssertTrackHeadAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "headHeight", headHeight_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
    if( !grabPng_.isEmpty() ) {
        elem.setAttribute( "grabPng", grabPng_ );
        if( grabWidth_ > 0 )  elem.setAttribute( "grabWidth", grabWidth_ );
        if( grabHeight_ > 0 ) elem.setAttribute( "grabHeight", grabHeight_ );
    }
}

bool SAssertTrackHeadAction::readXml( const QDomElement &elem, int )
{
    trackPath_  = elem.attribute( "trackPath", "0" );
    headHeight_ = elem.attribute( "headHeight", "160" ).toInt();
    contains_   = elem.attribute( "contains" );
    absent_     = elem.attribute( "absent" );
    grabPng_    = elem.attribute( "grabPng", "" );
    grabWidth_  = elem.attribute( "grabWidth", "0" ).toInt();
    grabHeight_ = elem.attribute( "grabHeight", "0" ).toInt();
    return true;
}

// --------------------------------------------------------------- registry

static const bool s_reg_virtualkey =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "virtual-key" ),
          [] { return new SVirtualKeyAction; } ),
      true );

static const bool s_reg_dragnote =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "drag-note" ),
          [] { return new SDragNoteAction; } ),
      true );

static const bool s_reg_asserteventeditor =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-event-editor" ),
          [] { return new SAssertEventEditorAction; } ),
      true );

static const bool s_reg_asserttrackhead =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-track-head" ),
          [] { return new SAssertTrackHeadAction; } ),
      true );
