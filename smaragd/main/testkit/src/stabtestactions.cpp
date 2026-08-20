#include "app/testkit/stabtestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/sviewtabs.h"
#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QStringList>

// The shell lives under the main window, reached the way every other testkit
// gesture verb reaches it.
static SViewTabs *viewTabs()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) )
            return win->ensureViewShell();
    }
    return nullptr;
}

SApplyResult SAssertTabSetAction::apply( SProject * /*project*/ )
{
    SViewTabs *tabs = viewTabs();
    if( !tabs ) {
        qWarning() << "assert-tab-set: no view shell";
        return { false, nullptr };
    }

    const QStringList actual = tabs->tabNames();

    if( !names_.isEmpty() || namesGiven_ ) {
        QStringList expected = names_.split( ',', Qt::SkipEmptyParts );
        for( QString &e : expected ) e = e.trimmed();
        // ORDER matters here, unlike assert-arrangements: tab 0 is the master
        // and the shell's whole contract is that it stays tab 0.
        if( expected != actual ) {
            qWarning() << "assert-tab-set FAILED: expected" << expected
                       << "but got" << actual;
            return { false, nullptr };
        }
    }

    if( !active_.isEmpty() ) {
        const QString have = tabs->activeTabName();
        if( have != active_ ) {
            qWarning() << "assert-tab-set FAILED: active tab is" << have
                       << "expected" << active_;
            return { false, nullptr };
        }
    }

    return { true, nullptr };
}

void SAssertTabSetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "names", names_ );
    if( !active_.isEmpty() ) elem.setAttribute( "active", active_ );
}

bool SAssertTabSetAction::readXml( const QDomElement &elem, int /*version*/ )
{
    namesGiven_ = elem.hasAttribute( "names" );
    names_  = elem.attribute( "names" );
    active_ = elem.attribute( "active" );
    return true;
}

static const bool s_reg_tabtest = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-tab-set"),
        []{ return new SAssertTabSetAction; } ),
    true
);
