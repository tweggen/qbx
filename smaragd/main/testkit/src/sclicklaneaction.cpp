#include "app/testkit/sclicklaneaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QStringList>

// The arranger lives under the main window, and testkit may not include
// app/timeline (tools/check_layering.py) — so both verbs reach it through the
// shell, the same route drag-clip-edge / select-track use.
static SMainWindow *mainWindow_()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

static bool parseModifiers_( const QString &modStr, Qt::KeyboardModifiers &out,
                             const char *who )
{
    out = Qt::NoModifier;
    const QString s = modStr.trimmed();
    if( s.isEmpty() ) return true;
    const QStringList parts = s.split( '+', Qt::SkipEmptyParts );
    for( const QString &raw : parts ) {
        const QString m = raw.trimmed().toLower();
        if( m == "ctrl" )       out |= Qt::ControlModifier;
        else if( m == "shift" ) out |= Qt::ShiftModifier;
        else if( m == "alt" )   out |= Qt::AltModifier;
        else {
            qWarning() << who << ": unknown modifier:" << raw;
            return false;
        }
    }
    return true;
}

// --- click-lane --------------------------------------------------------

SApplyResult SClickLaneAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow_();
    if( !win ) {
        qWarning() << "click-lane: no main window";
        return { false, nullptr };
    }
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    if( !parseModifiers_( modifiers_, mods, "click-lane" ) ) {
        return { false, nullptr };
    }
    if( !win->clickLane( trackPath_, time_, mods ) ) {
        qWarning() << "click-lane: no track at" << trackPath_;
        return { false, nullptr };
    }
    // Track/clip selection is view state; a press+release with no movement
    // never arms a move/duplicate/resize gesture, so nothing else here needs
    // an inverse.
    return { true, nullptr };
}

void SClickLaneAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "time", QString::number( (qint64) time_ ) );
    if( !modifiers_.isEmpty() ) elem.setAttribute( "modifiers", modifiers_ );
}

bool SClickLaneAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    bool ok = false;
    time_ = (offset_t) elem.attribute( "time", "0" ).toLongLong( &ok );
    if( !ok || time_ < 0 ) {
        qWarning() << "click-lane: invalid time:" << elem.attribute( "time" );
        return false;
    }
    modifiers_ = elem.attribute( "modifiers", "" );
    Qt::KeyboardModifiers ignored;
    return parseModifiers_( modifiers_, ignored, "click-lane" );
}

// --- split-selection -----------------------------------------------------

SApplyResult SSplitSelectionAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow_();
    if( !win ) {
        qWarning() << "split-selection: no main window";
        return { false, nullptr };
    }
    if( !win->splitSelectionGesture() ) {
        qWarning() << "split-selection: no arranger";
        return { false, nullptr };
    }
    // ctSplitSample() submits its own SCompositeAction of SSplitClipAction
    // children (or nothing at all, when no selected clip qualified); undoing
    // THAT reverses the gesture.
    return { true, nullptr };
}

void SSplitSelectionAction::writeXml( QDomElement & /*elem*/ ) const
{
}

bool SSplitSelectionAction::readXml( const QDomElement & /*elem*/, int /*version*/ )
{
    return true;
}

static const bool s_reg_clicklane =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "click-lane" ),
          [] { return new SClickLaneAction; } ),
      true );

static const bool s_reg_splitselection =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "split-selection" ),
          [] { return new SSplitSelectionAction; } ),
      true );
