#include "app/testkit/sarrangementtabactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/sviewtabs.h"
#include "app/model/sproject.h"
#include <QApplication>
#include <QDebug>
#include <QDomElement>

static SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

// --- open-arrangement-tab ---------------------------------------------------

SApplyResult SOpenArrangementTabAction::apply( SProject *project )
{
    SMainWindow *win = mainWindow();
    if( !win || !project ) {
        qWarning() << "open-arrangement-tab: no main window";
        return { false, nullptr };
    }
    SObject *root = project->arrangement( arrName_ );
    if( !root ) {
        qWarning() << "open-arrangement-tab: no arrangement named" << arrName_;
        return { false, nullptr };
    }
    SViewTabs *tabs = win->ensureViewShell();
    if( !tabs || !tabs->openFor( root, arrName_ ) ) {
        qWarning() << "open-arrangement-tab: could not open a tab for" << arrName_;
        return { false, nullptr };
    }
    // NOT undoable: opening a window is not an edit to the arrangement
    // (proposal 09 D2's split, and the same reasoning that keeps set-count-in
    // off the undo stack).
    return { true, nullptr };
}

void SOpenArrangementTabAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "name", arrName_ );
}

bool SOpenArrangementTabAction::readXml( const QDomElement &elem, int )
{
    arrName_ = elem.attribute( "name" );
    return true;
}

// --- close-arrangement-tab --------------------------------------------------

SApplyResult SCloseArrangementTabAction::apply( SProject *project )
{
    SMainWindow *win = mainWindow();
    if( !win || !project ) return { false, nullptr };
    SObject *root = project->arrangement( arrName_ );
    if( !root ) {
        qWarning() << "close-arrangement-tab: no arrangement named" << arrName_;
        return { false, nullptr };
    }
    if( SViewTabs *tabs = win->viewTabs() ) tabs->closeFor( root );
    return { true, nullptr };
}

void SCloseArrangementTabAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "name", arrName_ );
}

bool SCloseArrangementTabAction::readXml( const QDomElement &elem, int )
{
    arrName_ = elem.attribute( "name" );
    return true;
}

// --- activate-tab -----------------------------------------------------------

SApplyResult SActivateTabAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    SViewTabs *tabs = win ? win->ensureViewShell() : nullptr;
    if( !tabs ) return { false, nullptr };
    const QStringList names = tabs->tabNames();
    const int idx = names.indexOf( tabName_ );
    if( idx < 0 ) {
        qWarning() << "activate-tab: no tab named" << tabName_ << "; have" << names;
        return { false, nullptr };
    }
    tabs->setCurrentIndex( idx );
    return { true, nullptr };
}

void SActivateTabAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "name", tabName_ );
}

bool SActivateTabAction::readXml( const QDomElement &elem, int )
{
    tabName_ = elem.attribute( "name" );
    return true;
}

static const bool s_reg_tabverbs = (
    SActionRegistry::instance().registerType(
        QStringLiteral("open-arrangement-tab"),
        []{ return new SOpenArrangementTabAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("close-arrangement-tab"),
        []{ return new SCloseArrangementTabAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("activate-tab"),
        []{ return new SActivateTabAction; } ),
    true
);
