#include "app/testkit/stabtestactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/shell/sapplication.h"
#include "app/model/sobjectpath.h"
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

// --- assert-selection -------------------------------------------------------

SApplyResult SAssertSelectionAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SApplication &app = SApplication::app();
    const QString root = app.activeSelectionRoot();

    QStringList actual;
    for( const QList<int> &p : app.getCurrentSelectionPathsFor( root ) )
        actual << strackpath::qualifiedToString( root, p );
    actual.sort();

    QStringList expected = paths_.split( ';', Qt::SkipEmptyParts );
    for( QString &e : expected ) e = e.trimmed();
    expected.sort();

    if( expected != actual ) {
        qWarning() << "assert-selection FAILED: active root"
                   << ( root.isEmpty() ? QStringLiteral( "<master>" ) : root )
                   << "has" << actual << "expected" << expected;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertSelectionAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "paths", paths_ );
}

bool SAssertSelectionAction::readXml( const QDomElement &elem, int )
{
    paths_ = elem.attribute( "paths" );
    return true;
}

// --- assert-view-playhead ---------------------------------------------------

SApplyResult SAssertViewPlayheadAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    // Through the SHELL: testkit may not include app/timeline (testkit
    // CONTRACT inv. 5), the same route drag-clip-edge and assert-envelope take.
    SMainWindow *win = nullptr;
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( SMainWindow *m = qobject_cast<SMainWindow *>( w ) ) { win = m; break; }
    if( !win ) {
        qWarning() << "assert-view-playhead: no main window";
        return { false, nullptr };
    }
    win->ensureViewShell();

    struct { offset_t pos = 0; bool sounding = false; } lp;
    if( !win->viewPlayheadFor( root_, lp.pos, lp.sounding ) ) {
        qWarning() << "assert-view-playhead FAILED: no open view for root" << root_
                   << "(open-arrangement-tab first; this verb opens nothing)";
        return { false, nullptr };
    }

    if( !sounding_.isEmpty() ) {
        const bool want = ( sounding_ == QStringLiteral("true") );
        if( lp.sounding != want ) {
            qWarning() << "assert-view-playhead FAILED: root" << root_
                       << "sounding is" << lp.sounding << "expected" << want
                       << "(pos" << (qint64) lp.pos << ")";
            return { false, nullptr };
        }
    }
    if( frameGiven_ ) {
        const qint64 have = (qint64) lp.pos;
        if( qAbs( have - frame_ ) > tolerance_ ) {
            qWarning() << "assert-view-playhead FAILED: root" << root_
                       << "frame is" << have << "expected" << frame_
                       << "tolerance" << tolerance_;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SAssertViewPlayheadAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "root", root_ );
    if( frameGiven_ ) elem.setAttribute( "frame", QString::number( frame_ ) );
    if( !sounding_.isEmpty() ) elem.setAttribute( "sounding", sounding_ );
    // Always written: the roundtrip gate compares attribute sets, and a
    // conditional write of a value the fixture spells is a mismatch.
    elem.setAttribute( "tolerance", QString::number( tolerance_ ) );
}

bool SAssertViewPlayheadAction::readXml( const QDomElement &elem, int )
{
    root_       = elem.attribute( "root" );
    frameGiven_ = elem.hasAttribute( "frame" );
    frame_      = elem.attribute( "frame", "0" ).toLongLong();
    sounding_   = elem.attribute( "sounding" );
    tolerance_  = elem.attribute( "tolerance", "0" ).toLongLong();
    return true;
}

static const bool s_reg_tabtest = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-tab-set"),
        []{ return new SAssertTabSetAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-selection"),
        []{ return new SAssertSelectionAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-view-playhead"),
        []{ return new SAssertViewPlayheadAction; } ),
    true
);
