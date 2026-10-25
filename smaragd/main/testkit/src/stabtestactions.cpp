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
    const QString root = hasRoot_ ? root_ : app.activeSelectionRoot();

    // EVERY root, before anything else and whatever this element asserts: a
    // `current` left pointing at a link its own list no longer holds is a
    // cross-root defect, and an assertion about selection is the cheapest
    // place to notice it. Pointer comparisons only -- see the SApplication
    // declaration. QBX-230.
    QString badRoot;
    if( !app.selectionCurrentIsConsistent( &badRoot ) ) {
        qWarning() << "assert-selection FAILED: root"
                   << ( badRoot.isEmpty() ? QStringLiteral( "<master>" ) : badRoot )
                   << "has a `current` that its own selection list does not"
                      " hold -- a destroyed link was dropped from the list but"
                      " left in `current`";
        return { false, nullptr };
    }

    // SIZE FIRST, and it is the only check when `paths` is absent: see the
    // header. A count needs no dereference, so it still answers on a root
    // holding a stale pointer -- which is the whole point for QBX-230.
    if( hasCount_ ) {
        const int actualCount = app.selectionListFor( root ).count();
        if( actualCount != count_ ) {
            qWarning() << "assert-selection FAILED: root"
                       << ( root.isEmpty() ? QStringLiteral( "<master>" ) : root )
                       << "holds" << actualCount << "link(s), expected" << count_;
            return { false, nullptr };
        }
        if( paths_.isEmpty() ) return { true, nullptr };
    }

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
    if( hasRoot_ )  elem.setAttribute( "root", root_ );
    if( hasCount_ ) elem.setAttribute( "count", count_ );
}

bool SAssertSelectionAction::readXml( const QDomElement &elem, int )
{
    paths_    = elem.attribute( "paths" );
    hasRoot_  = elem.hasAttribute( "root" );
    root_     = elem.attribute( "root" );
    hasCount_ = elem.hasAttribute( "count" );
    count_    = elem.attribute( "count", "0" ).toInt();
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
