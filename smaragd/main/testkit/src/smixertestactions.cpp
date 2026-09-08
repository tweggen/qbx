#include "app/testkit/smixertestactions.h"

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

int fieldOf( const QString &desc, const QString &key )
{
    for( const QString &part : desc.split( QLatin1Char( '|' ) ) )
        if( part.startsWith( key + QLatin1Char( '=' ) ) )
            return part.mid( key.size() + 1 ).toInt();
    return -1;
}

}  // namespace

// --- assert-mixer-pane ------------------------------------------------------

SApplyResult SAssertMixerPaneAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "assert-mixer-pane: no main window"; return { false, nullptr }; }

    const QString desc = track_.isEmpty()
                             ? win->describeMixerPane( arrangement_ )
                             : win->describeMixerStrip( track_, arrangement_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-mixer-pane: no description for"
                   << ( track_.isEmpty() ? QStringLiteral( "the pane" ) : track_ );
        return { false, nullptr };
    }

    if( strips_ >= 0 ) {
        const int got = fieldOf( desc, QStringLiteral( "strips" ) );
        if( got != strips_ ) {
            qWarning() << "assert-mixer-pane FAILED: strips" << got
                       << "expected" << strips_ << "-" << desc;
            return { false, nullptr };
        }
    }
    if( master_ >= 0 ) {
        const int got = fieldOf( desc, QStringLiteral( "master" ) );
        if( got != master_ ) {
            qWarning() << "assert-mixer-pane FAILED: master" << got
                       << "expected" << master_ << "-" << desc;
            return { false, nullptr };
        }
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-mixer-pane FAILED: missing" << contains_ << "-" << desc;
        return { false, nullptr };
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        qWarning() << "assert-mixer-pane FAILED: unexpected" << absent_ << "-" << desc;
        return { false, nullptr };
    }
    qDebug() << "assert-mixer-pane: OK -" << desc;
    return { true, nullptr };
}

QStringList SAssertMixerPaneAction::knownAttributes() const
{
    return { QStringLiteral( "track" ), QStringLiteral( "arrangement" ),
             QStringLiteral( "strips" ), QStringLiteral( "master" ),
             QStringLiteral( "contains" ), QStringLiteral( "absent" ) };
}

void SAssertMixerPaneAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "arrangement", arrangement_ );
    elem.setAttribute( "strips", strips_ );
    elem.setAttribute( "master", master_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
}

bool SAssertMixerPaneAction::readXml( const QDomElement &elem, int )
{
    track_       = elem.attribute( "track", QString() );
    arrangement_ = elem.attribute( "arrangement", QString() );
    strips_      = elem.attribute( "strips", "-1" ).toInt();
    master_   = elem.attribute( "master", "-1" ).toInt();
    contains_ = elem.attribute( "contains", QString() );
    absent_   = elem.attribute( "absent", QString() );
    return true;
}

// --- assert-mixer-layout ----------------------------------------------------

SApplyResult SAssertMixerLayoutAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "assert-mixer-layout: no main window"; return { false, nullptr }; }

    const QString desc =
        win->describeMixerLayout( paneWidth_, paneHeight_, stripWidth_ );
    if( desc.isEmpty() ) {
        qWarning() << "assert-mixer-layout: no pane";
        return { false, nullptr };
    }

    const int crushed = fieldOf( desc, QStringLiteral( "crushed" ) );
    const int overlap = fieldOf( desc, QStringLiteral( "overlap" ) );
    if( crushed > maxCrushed_ ) {
        qWarning() << "assert-mixer-layout FAILED:" << crushed
                   << "widget(s) given less space than they need, max"
                   << maxCrushed_ << "-" << desc;
        return { false, nullptr };
    }
    if( overlap > maxOverlap_ ) {
        qWarning() << "assert-mixer-layout FAILED:" << overlap
                   << "overlapping pair(s), max" << maxOverlap_ << "-" << desc;
        return { false, nullptr };
    }
    if( scrollNeeded_ >= 0 ) {
        const int got = fieldOf( desc, QStringLiteral( "scrollNeeded" ) );
        if( got != scrollNeeded_ ) {
            qWarning() << "assert-mixer-layout FAILED: scrollNeeded" << got
                       << "expected" << scrollNeeded_ << "-" << desc;
            return { false, nullptr };
        }
    }
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-mixer-layout FAILED: missing" << contains_ << "-" << desc;
        return { false, nullptr };
    }
    qDebug() << "assert-mixer-layout: OK -" << desc;
    return { true, nullptr };
}

QStringList SAssertMixerLayoutAction::knownAttributes() const
{
    return { QStringLiteral( "paneWidth" ), QStringLiteral( "paneHeight" ),
             QStringLiteral( "stripWidth" ), QStringLiteral( "maxCrushed" ),
             QStringLiteral( "maxOverlap" ), QStringLiteral( "scrollNeeded" ),
             QStringLiteral( "contains" ) };
}

void SAssertMixerLayoutAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "paneWidth", paneWidth_ );
    elem.setAttribute( "paneHeight", paneHeight_ );
    elem.setAttribute( "stripWidth", stripWidth_ );
    elem.setAttribute( "maxCrushed", maxCrushed_ );
    elem.setAttribute( "maxOverlap", maxOverlap_ );
    elem.setAttribute( "scrollNeeded", scrollNeeded_ );
    elem.setAttribute( "contains", contains_ );
}

bool SAssertMixerLayoutAction::readXml( const QDomElement &elem, int )
{
    paneWidth_    = elem.attribute( "paneWidth", "640" ).toInt();
    paneHeight_   = elem.attribute( "paneHeight", "260" ).toInt();
    stripWidth_   = elem.attribute( "stripWidth", "96" ).toInt();
    maxCrushed_   = elem.attribute( "maxCrushed", "0" ).toInt();
    maxOverlap_   = elem.attribute( "maxOverlap", "0" ).toInt();
    scrollNeeded_ = elem.attribute( "scrollNeeded", "-1" ).toInt();
    contains_     = elem.attribute( "contains", QString() );
    return true;
}

// --- mixer-strip-toggle -----------------------------------------------------

SApplyResult SMixerStripToggleAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "mixer-strip-toggle: no main window"; return { false, nullptr }; }
    if( !win->mixerStripToggle( track_, control_, on_, arrangement_ ) ) {
        qWarning() << "mixer-strip-toggle FAILED:" << track_ << control_ << on_;
        return { false, nullptr };
    }
    qDebug() << "mixer-strip-toggle: OK -" << track_ << control_ << on_;
    // A GESTURE verb adds no undo step of its own: the control's own handler
    // submits the real action, which is the property that makes a missing
    // connect() fail rather than pass silently.
    return { true, nullptr };
}

QStringList SMixerStripToggleAction::knownAttributes() const
{
    return { QStringLiteral( "track" ), QStringLiteral( "arrangement" ),
             QStringLiteral( "control" ), QStringLiteral( "on" ) };
}

void SMixerStripToggleAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "arrangement", arrangement_ );
    elem.setAttribute( "control", control_ );
    elem.setAttribute( "on", on_ ? "true" : "false" );
}

bool SMixerStripToggleAction::readXml( const QDomElement &elem, int )
{
    track_       = elem.attribute( "track", QString() );
    arrangement_ = elem.attribute( "arrangement", QString() );
    control_     = elem.attribute( "control", "mute" );
    on_      = elem.attribute( "on", "true" ) == QLatin1String( "true" );
    return !track_.isEmpty() && !control_.isEmpty();
}

// --- registration -----------------------------------------------------------

static const bool s_reg_assert_mixer_pane = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-mixer-pane" ),
        []{ return new SAssertMixerPaneAction; } ), true );

static const bool s_reg_assert_mixer_layout = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-mixer-layout" ),
        []{ return new SAssertMixerLayoutAction; } ), true );

static const bool s_reg_mixer_strip_toggle = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "mixer-strip-toggle" ),
        []{ return new SMixerStripToggleAction; } ), true );
