#include "app/testkit/spurgeunusedclipstestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/model/sunusedclips.h"
#include "app/shell/smainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>

static SMainWindow *mainWindow_()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

// --- assert-unused-clips ---------------------------------------------------

SApplyResult SAssertUnusedClipsAction::apply( SProject *project )
{
    if( !project ) {
        qWarning() << "assert-unused-clips FAILED: no project";
        return { false, nullptr };
    }
    const QString d = sunusedclips::describe( project );
    const int n = sunusedclips::list( project ).size();

    bool ok = true;
    if( count_ >= 0 && n != count_ ) {
        qWarning() << "assert-unused-clips FAILED: count is" << n
                   << "expected" << count_ << "-" << d;
        ok = false;
    }
    if( !contains_.isEmpty() && !d.contains( contains_ ) ) {
        qWarning() << "assert-unused-clips FAILED: no" << contains_
                   << "in" << d;
        ok = false;
    }
    if( !absent_.isEmpty() && d.contains( absent_ ) ) {
        qWarning() << "assert-unused-clips FAILED:" << absent_
                   << "must be absent from" << d;
        ok = false;
    }
    if( menuEnabled_ >= 0 ) {
        SMainWindow *win = mainWindow_();
        if( !win ) {
            qWarning() << "assert-unused-clips FAILED: menuEnabled= needs the "
                          "main window";
            ok = false;
        } else {
            const int en = win->purgeUnusedClipsMenuEnabled() ? 1 : 0;
            if( en != menuEnabled_ ) {
                qWarning() << "assert-unused-clips FAILED: menu item enabled"
                           << en << "expected" << menuEnabled_ << "-" << d;
                ok = false;
            }
        }
    }
    if( ok ) qDebug() << "assert-unused-clips: OK -" << d;
    return { ok, nullptr };
}

void SAssertUnusedClipsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "count", count_ );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    if( !absent_.isEmpty() )   elem.setAttribute( "absent", absent_ );
    if( menuEnabled_ >= 0 )    elem.setAttribute( "menuEnabled", menuEnabled_ );
}

bool SAssertUnusedClipsAction::readXml( const QDomElement &elem, int )
{
    count_       = elem.attribute( "count", "-1" ).toInt();
    contains_    = elem.attribute( "contains", "" );
    absent_      = elem.attribute( "absent", "" );
    menuEnabled_ = elem.attribute( "menuEnabled", "-1" ).toInt();
    return true;
}

static const bool s_reg_assert_unused_clips = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-unused-clips" ),
        []{ return new SAssertUnusedClipsAction; } ), true );

// --- purge-unused-clips-gesture ----------------------------------------------

SApplyResult SPurgeUnusedClipsGestureAction::apply( SProject * )
{
    SMainWindow *win = mainWindow_();
    if( !win ) {
        qWarning() << "purge-unused-clips-gesture: no main window";
        return { false, nullptr };
    }

    if( via_ == QLatin1String( "menu" ) ) {
        const bool fired = win->triggerPurgeUnusedClipsMenu();
        if( fired == expectRefused_ ) {
            qWarning() << "purge-unused-clips-gesture FAILED: the menu item"
                       << ( fired ? "fired" : "was REFUSED (greyed)" )
                       << "- expected"
                       << ( expectRefused_ ? "a refusal" : "it to fire" );
            return { false, nullptr };
        }
        // What it released is asserted with assert-unused-clips: the menu's
        // slot returns nothing, as a QAction's does.
        qDebug() << "purge-unused-clips-gesture: menu"
                 << ( fired ? "fired" : "refused" );
        return { true, nullptr };
    }
    if( via_ != QLatin1String( "rows" ) ) {
        qWarning() << "purge-unused-clips-gesture: unknown via=" << via_;
        return { false, nullptr };
    }

    QList<int> rows;
    for( const QString &raw : rows_.split( ';', Qt::SkipEmptyParts ) ) {
        bool ok = false;
        const int r = raw.trimmed().toInt( &ok );
        if( !ok ) {
            qWarning() << "purge-unused-clips-gesture: bad row" << raw;
            return { false, nullptr };
        }
        rows << r;
    }
    const int purged = win->purgeUnusedClipsGesture( &rows );
    if( expectPurged_ >= 0 && purged != expectPurged_ ) {
        qWarning() << "purge-unused-clips-gesture FAILED: released" << purged
                   << "clip(s), expected" << expectPurged_ << "- rows" << rows_;
        return { false, nullptr };
    }
    qDebug() << "purge-unused-clips-gesture: rows" << rows_ << "released"
             << purged;
    // The gesture submits its own purge-unused-clips; nothing to invert here.
    return { true, nullptr };
}

void SPurgeUnusedClipsGestureAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "via", via_ );
    if( !rows_.isEmpty() ) elem.setAttribute( "rows", rows_ );
    if( expectPurged_ >= 0 ) elem.setAttribute( "expectPurged", expectPurged_ );
    if( expectRefused_ ) elem.setAttribute( "expectRefused", "1" );
}

bool SPurgeUnusedClipsGestureAction::readXml( const QDomElement &elem, int )
{
    via_           = elem.attribute( "via", "rows" );
    rows_          = elem.attribute( "rows", "" );
    expectPurged_  = elem.attribute( "expectPurged", "-1" ).toInt();
    expectRefused_ = elem.attribute( "expectRefused", "0" ) != QLatin1String( "0" );
    return true;
}

static const bool s_reg_purge_unused_clips_gesture = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "purge-unused-clips-gesture" ),
        []{ return new SPurgeUnusedClipsGestureAction; } ), true );
