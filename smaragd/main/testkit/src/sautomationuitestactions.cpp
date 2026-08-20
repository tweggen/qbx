#include "app/testkit/sautomationuitestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sautomationlane.h"
#include "app/model/sobject.h"
#include "app/model/sobjectpath.h"
#include "app/objects/track/sautomationactions.h"
#include "app/shell/sapplication.h"
#include "app/shell/sautomationrecorder.h"
#include "app/shell/smainwindow.h"
#include "tw/core/twfraction.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>

using namespace strackpath;

// The arranger lives under the main window, and testkit may not include
// app/timeline (tools/check_layering.py, testkit CONTRACT inv. 5) — so the
// gesture verb reaches it through the shell, the same route drag-clip-edge and
// drag-note use.
static SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

// "ctrl+alt" -> the flags. The same spelling drag-clip-edge and drag-note take;
// an unknown token is REJECTED rather than ignored, because a silently dropped
// modifier turns a delete gesture into an add and the case would still pass.
static bool parseMods( const QString &spec, Qt::KeyboardModifiers &out )
{
    out = Qt::NoModifier;
    const QString s = spec.trimmed();
    if( s.isEmpty() ) return true;
    const QStringList parts = s.split( QLatin1Char( '+' ), Qt::SkipEmptyParts );
    for( const QString &raw : parts ) {
        const QString t = raw.trimmed().toLower();
        if( t == QLatin1String( "ctrl" ) )       out |= Qt::ControlModifier;
        else if( t == QLatin1String( "alt" ) )   out |= Qt::AltModifier;
        else if( t == QLatin1String( "shift" ) ) out |= Qt::ShiftModifier;
        else if( t == QLatin1String( "meta" ) )  out |= Qt::MetaModifier;
        else {
            qWarning() << "drag-automation-point: unknown modifier" << raw;
            return false;
        }
    }
    return true;
}

static QString modsToString( Qt::KeyboardModifiers m )
{
    QStringList out;
    if( m & Qt::ControlModifier ) out << QStringLiteral( "ctrl" );
    if( m & Qt::AltModifier )     out << QStringLiteral( "alt" );
    if( m & Qt::ShiftModifier )   out << QStringLiteral( "shift" );
    if( m & Qt::MetaModifier )    out << QStringLiteral( "meta" );
    return out.join( QLatin1Char( '+' ) );
}

// --- drag-automation-point ---------------------------------------------------

SApplyResult SDragAutomationPointAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "drag-automation-point: no main window";
        return { false, nullptr };
    }
    if( target_.isEmpty() ) {
        qWarning() << "drag-automation-point: no target";
        return { false, nullptr };
    }
    if( !win->dragAutomationPoint( pathToString( ownerPath_ ), target_,
                                   slotIndex_, take_, time_, value_,
                                   hasTo_ ? toTime_ : time_,
                                   hasTo_ ? toValue_ : value_, mods_ ) ) {
        qWarning() << "drag-automation-point: could not aim at" << target_
                   << "on" << pathToString( ownerPath_ );
        return { false, nullptr };
    }
    // The gesture submits its OWN verb on press or release (add / move /
    // remove / set-automation-points); undoing THAT is what reverses it.
    return { true, nullptr };
}

void SDragAutomationPointAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "owner", pathToString( ownerPath_ ) );
    elem.setAttribute( "target", target_ );
    elem.setAttribute( "time", QString::number( (qlonglong) time_ ) );
    elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
    if( slotIndex_ >= 0 ) elem.setAttribute( "slotIndex", slotIndex_ );
    if( take_ >= 0 )      elem.setAttribute( "take", take_ );
    if( hasTo_ ) {
        elem.setAttribute( "toTime", QString::number( (qlonglong) toTime_ ) );
        elem.setAttribute( "toValue", QString::number( toValue_, 'g', 17 ) );
    }
    const QString m = modsToString( mods_ );
    if( !m.isEmpty() ) elem.setAttribute( "modifiers", m );
}

bool SDragAutomationPointAction::readXml( const QDomElement &elem, int )
{
    ownerPath_ = parseInto( pathRoot_, elem.attribute( "owner" ) );
    target_    = elem.attribute( "target" );
    slotIndex_ = elem.attribute( "slotIndex", "-1" ).toInt();
    take_      = elem.attribute( "take", "-1" ).toInt();
    time_      = (offset_t) parseFractionOrDouble(
                     elem.attribute( "time", "0" ).toStdString() ).toDouble();
    value_     = elem.attribute( "value", "0" ).toDouble();
    // BOTH must be present for a move: a `toTime` without a `toValue` would
    // silently drag the point to value 0 and the case would be asserting the
    // wrong thing.
    hasTo_ = elem.hasAttribute( "toTime" ) || elem.hasAttribute( "toValue" );
    if( hasTo_ ) {
        toTime_  = (offset_t) parseFractionOrDouble(
                       elem.attribute( "toTime", "0" ).toStdString() ).toDouble();
        toValue_ = elem.attribute( "toValue", "0" ).toDouble();
    }
    if( target_.isEmpty() ) {
        qWarning() << "drag-automation-point: `target` is required";
        return false;
    }
    return parseMods( elem.attribute( "modifiers", "" ), mods_ );
}

QStringList SDragAutomationPointAction::knownAttributes() const
{
    return { QStringLiteral( "owner" ),     QStringLiteral( "target" ),
             QStringLiteral( "slotIndex" ), QStringLiteral( "take" ),
             QStringLiteral( "time" ),      QStringLiteral( "value" ),
             QStringLiteral( "toTime" ),    QStringLiteral( "toValue" ),
             QStringLiteral( "modifiers" ) };
}

// --- automation-write-tick ---------------------------------------------------

SApplyResult SAutomationWriteTickAction::apply( SProject *project )
{
    if( target_.isEmpty() ) {
        qWarning() << "automation-write-tick: no target";
        return { false, nullptr };
    }
    sautomation::OwnerRef o = sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) {
        qWarning() << "automation-write-tick: cannot resolve owner"
                   << pathToString( ownerPath_ ) << "for target" << target_;
        return { false, nullptr };
    }

    SAutomationRecorder::Target t;
    t.ownerPath = ownerPath_;
    t.target    = target_;
    t.slotIndex = slotIndex_;
    t.take      = take_;

    SAutomationRecorder &rec = SApplication::app().automationRecorder();
    if( release_ ) {
        // Only meaningful while a pass is open; a bare release is a no-op the
        // case has no business writing, so say so.
        if( !rec.isActive() ) {
            qWarning() << "automation-write-tick release=1 with no open pass";
            return { false, nullptr };
        }
        rec.releaseControl();
        return { true, nullptr };
    }

    const offset_t frame =
        hasTime_ ? time_ : SApplication::app().getGlobalLocatorPos();
    if( !rec.writeTick( t, value_, frame ) ) {
        qWarning() << "automation-write-tick:" << target_ << "on"
                   << pathToString( ownerPath_ )
                   << "is not in a touch/latch/write mode (it is"
                   << sAutomationModeToString(
                          SAutomationRecorder::modeOf( t ) ) << ")";
        return { false, nullptr };
    }
    // NO INVERSE, on purpose: a tick is one sample of a live pass and the pass
    // commits ONE set-automation-points when it ends. That single action is the
    // undo step — see the class comment.
    return { true, nullptr };
}

void SAutomationWriteTickAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "owner", pathToString( ownerPath_ ) );
    elem.setAttribute( "target", target_ );
    elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
    if( slotIndex_ >= 0 ) elem.setAttribute( "slotIndex", slotIndex_ );
    if( take_ >= 0 )      elem.setAttribute( "take", take_ );
    if( hasTime_ )        elem.setAttribute( "time",
                                             QString::number( (qlonglong) time_ ) );
    if( release_ )        elem.setAttribute( "release", "1" );
}

bool SAutomationWriteTickAction::readXml( const QDomElement &elem, int )
{
    ownerPath_ = parseInto( pathRoot_, elem.attribute( "owner" ) );
    target_    = elem.attribute( "target" );
    slotIndex_ = elem.attribute( "slotIndex", "-1" ).toInt();
    take_      = elem.attribute( "take", "-1" ).toInt();
    value_     = elem.attribute( "value", "0" ).toDouble();
    hasTime_   = elem.hasAttribute( "time" );
    if( hasTime_ )
        time_ = (offset_t) parseFractionOrDouble(
                    elem.attribute( "time", "0" ).toStdString() ).toDouble();
    release_ = ( elem.attribute( "release", "0" ) == QLatin1String( "1" ) );
    if( target_.isEmpty() ) {
        qWarning() << "automation-write-tick: `target` is required";
        return false;
    }
    return true;
}

QStringList SAutomationWriteTickAction::knownAttributes() const
{
    return { QStringLiteral( "owner" ),     QStringLiteral( "target" ),
             QStringLiteral( "slotIndex" ), QStringLiteral( "take" ),
             QStringLiteral( "value" ),     QStringLiteral( "time" ),
             QStringLiteral( "release" ) };
}

static const bool s_reg_dragautomationpoint =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "drag-automation-point" ),
          [] { return new SDragAutomationPointAction; } ),
      true );

static const bool s_reg_automationwritetick =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "automation-write-tick" ),
          [] { return new SAutomationWriteTickAction; } ),
      true );
