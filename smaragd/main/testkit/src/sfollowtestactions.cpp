#include "app/testkit/sfollowtestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include <QApplication>
#include <QDebug>
#include <QDomElement>

// Through the SHELL: testkit may not include app/timeline (testkit CONTRACT
// inv. 5), the same route wheel-scroll and assert-view-playhead take.
static SMainWindow *mainWindow_()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

// --- advance-locator ---------------------------------------------------------

SApplyResult SAdvanceLocatorAction::apply( SProject * /*project*/ )
{
    SApplication &app = SApplication::app();
    if( app.isPlaying() || app.isRecordingActive() ) {
        qWarning() << "advance-locator: REJECTED, the transport is running and"
                      " the audio thread owns the locator";
        return { false, nullptr };
    }
    // The arranger must exist for its followLocator slot to be connected.
    if( SMainWindow *win = mainWindow_() ) win->ensureViewShell();
    app.tkPumpLocatorTo( position_ );
    return { true, nullptr };
}

void SAdvanceLocatorAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "position", QString::number( (qlonglong) position_ ) );
}

bool SAdvanceLocatorAction::readXml( const QDomElement &elem, int /*version*/ )
{
    bool ok = false;
    const qlonglong p = elem.attribute( "position", "0" ).toLongLong( &ok );
    if( !ok || p < 0 ) {
        qWarning() << "advance-locator: position must be a frame >= 0, got"
                   << elem.attribute( "position" );
        return false;
    }
    position_ = (offset_t) p;
    return true;
}

// --- advance-follow-clock ----------------------------------------------------

SApplyResult SAdvanceFollowClockAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow_();
    if( !win ) {
        qWarning() << "advance-follow-clock: no main window";
        return { false, nullptr };
    }
    if( !win->advanceArrangerFollowClock( ms_ ) ) {
        qWarning() << "advance-follow-clock: no arranger";
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAdvanceFollowClockAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "ms", QString::number( ms_ ) );
}

bool SAdvanceFollowClockAction::readXml( const QDomElement &elem, int /*version*/ )
{
    bool ok = false;
    const qlonglong v = elem.attribute( "ms", "0" ).toLongLong( &ok );
    if( !ok || v < 0 ) {
        qWarning() << "advance-follow-clock: ms must be >= 0, got"
                   << elem.attribute( "ms" );
        return false;
    }
    ms_ = (qint64) v;
    return true;
}

static const bool s_reg_followtest = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "advance-locator" ),
        [] { return new SAdvanceLocatorAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "advance-follow-clock" ),
        [] { return new SAdvanceFollowClockAction; } ),
    true );
