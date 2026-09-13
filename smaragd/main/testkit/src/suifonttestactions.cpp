#include "app/testkit/suifonttestactions.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"

namespace {

SMainWindow *mainWindow()
{
    SMainWindow *win = nullptr;
    // NOT QApplication::activeWindow(): a --test-case run never shows the
    // window, so nothing is ever "active" under QT_QPA_PLATFORM=offscreen.
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( ( win = qobject_cast<SMainWindow *>( w ) ) ) break;
    return win;
}

// contains / absent over one description, with a readable failure.
bool checkDescription( const char *verb, const QString &desc,
                       const QString &contains, const QString &absent )
{
    if( !contains.isEmpty() && !desc.contains( contains ) ) {
        qWarning().noquote() << verb << "FAILED: expected" << contains << "in" << desc;
        return false;
    }
    if( !absent.isEmpty() && desc.contains( absent ) ) {
        qWarning().noquote() << verb << "FAILED: did not expect" << absent << "in" << desc;
        return false;
    }
    qDebug().noquote() << verb << ": OK -" << desc;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------

SApplyResult SAssertUiFontsAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "assert-ui-fonts: no main window"; return { false, nullptr }; }
    const QString desc = win->describeUiFonts( trackPath_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-ui-fonts: no track at" << trackPath_;
        return { false, nullptr };
    }
    return { checkDescription( "assert-ui-fonts", desc, contains_, absent_ ), nullptr };
}

void SAssertUiFontsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
}

bool SAssertUiFontsAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    contains_  = elem.attribute( "contains" );
    absent_    = elem.attribute( "absent" );
    return true;
}

// ---------------------------------------------------------------------------

SApplyResult STrackDetailSectionAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "track-detail-section: no main window"; return { false, nullptr }; }
    const QString before = win->describeTrackDetailSections( trackPath_ );
    if( before.isEmpty() ) {
        qWarning() << "track-detail-section: no track at" << trackPath_;
        return { false, nullptr };
    }
    const QString token = section_ + QLatin1Char( '=' );
    const int at = before.indexOf( token );
    if( at < 0 ) {
        qWarning() << "track-detail-section: unknown section" << section_;
        return { false, nullptr };
    }
    const bool nowCollapsed = before.mid( at + token.size(), 1 ) == QLatin1String( "1" );
    // ABSOLUTE: click only when the panel does not already show the wanted
    // state, so a script says what it wants and not how many times to toggle.
    if( nowCollapsed != collapsed_ ) {
        if( !win->clickTrackDetailSection( trackPath_, section_ ) ) {
            qWarning() << "track-detail-section: header" << section_ << "could not be clicked";
            return { false, nullptr };
        }
    }
    qDebug() << "track-detail-section:" << section_ << "collapsed =" << collapsed_;
    return { true, nullptr };   // view state in a project property, not an undo step
}

void STrackDetailSectionAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "section", section_ );
    elem.setAttribute( "collapsed", collapsed_ ? "1" : "0" );
}

bool STrackDetailSectionAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    section_   = elem.attribute( "section", "feelflow" );
    const QString c = elem.attribute( "collapsed", "1" );
    collapsed_ = c.startsWith( '1' ) || c.startsWith( 't' );
    return true;
}

// ---------------------------------------------------------------------------

SApplyResult SAssertTrackDetailSectionsAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "assert-track-detail-sections: no main window"; return { false, nullptr }; }
    const QString desc = win->describeTrackDetailSections( trackPath_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-track-detail-sections: no track at" << trackPath_;
        return { false, nullptr };
    }
    return { checkDescription( "assert-track-detail-sections", desc, contains_, absent_ ),
             nullptr };
}

void SAssertTrackDetailSectionsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
}

bool SAssertTrackDetailSectionsAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    contains_  = elem.attribute( "contains" );
    absent_    = elem.attribute( "absent" );
    return true;
}

static const bool s_reg_assert_ui_fonts = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-ui-fonts" ),
        []{ return new SAssertUiFontsAction; } ), true );

static const bool s_reg_track_detail_section = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "track-detail-section" ),
        []{ return new STrackDetailSectionAction; } ), true );

static const bool s_reg_assert_track_detail_sections = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-track-detail-sections" ),
        []{ return new SAssertTrackDetailSectionsAction; } ), true );
