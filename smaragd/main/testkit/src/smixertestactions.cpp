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
    if( minMeterDb_ < 1e29 || maxMeterDb_ < 1e29 ) {
        const int at = desc.indexOf( QStringLiteral( "|db=" ),
                                     desc.indexOf( QStringLiteral( "|meter=" ) ) );
        if( at < 0 ) {
            qWarning() << "assert-mixer-pane FAILED: no meter dB -" << desc;
            return { false, nullptr };
        }
        const double db = desc.mid( at + 4 )
                              .section( QLatin1Char( '|' ), 0, 0 )
                              .section( QLatin1Char( ',' ), 0, 0 ).toDouble();
        if( minMeterDb_ < 1e29 && db < minMeterDb_ ) {
            qWarning() << "assert-mixer-pane FAILED: meter" << db << "dB <"
                       << minMeterDb_ << "-" << desc;
            return { false, nullptr };
        }
        if( maxMeterDb_ < 1e29 && db > maxMeterDb_ ) {
            qWarning() << "assert-mixer-pane FAILED: meter" << db << "dB >"
                       << maxMeterDb_ << "-" << desc;
            return { false, nullptr };
        }
    }
    if( minLaneDbDelta_ >= 0.0 ) {
        // The meter's own `db=a,b` field, which is the widget's ballistics
        // rather than the probe's raw sample -- deliberately: what this gates
        // is what a user SEES on the strip.
        const int at = desc.indexOf( QStringLiteral( "|db=" ),
                                     desc.indexOf( QStringLiteral( "|meter=" ) ) );
        const QString tail = at < 0 ? QString() : desc.mid( at + 4 );
        const QStringList lanes =
            tail.section( QLatin1Char( '|' ), 0, 0 ).split( QLatin1Char( ',' ) );
        if( lanes.size() < 2 ) {
            qWarning() << "assert-mixer-pane FAILED: fewer than two meter lanes"
                       << "-" << desc;
            return { false, nullptr };
        }
        const double delta = qAbs( lanes[0].toDouble() - lanes[1].toDouble() );
        if( delta < minLaneDbDelta_ ) {
            qWarning() << "assert-mixer-pane FAILED: lane dB" << lanes[0]
                       << "vs" << lanes[1] << "differ by" << delta
                       << "which is below" << minLaneDbDelta_ << "-" << desc;
            return { false, nullptr };
        }
    }
    qDebug() << "assert-mixer-pane: OK -" << desc;
    return { true, nullptr };
}

QStringList SAssertMixerPaneAction::knownAttributes() const
{
    return { QStringLiteral( "track" ), QStringLiteral( "arrangement" ),
             QStringLiteral( "strips" ), QStringLiteral( "master" ),
             QStringLiteral( "contains" ), QStringLiteral( "absent" ),
             QStringLiteral( "minLaneDbDelta" ), QStringLiteral( "minMeterDb" ),
             QStringLiteral( "maxMeterDb" ) };
}

void SAssertMixerPaneAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "arrangement", arrangement_ );
    elem.setAttribute( "strips", strips_ );
    elem.setAttribute( "master", master_ );
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
    elem.setAttribute( "minLaneDbDelta", minLaneDbDelta_ );
    elem.setAttribute( "minMeterDb", minMeterDb_ );
    elem.setAttribute( "maxMeterDb", maxMeterDb_ );
}

bool SAssertMixerPaneAction::readXml( const QDomElement &elem, int )
{
    track_       = elem.attribute( "track", QString() );
    arrangement_ = elem.attribute( "arrangement", QString() );
    strips_      = elem.attribute( "strips", "-1" ).toInt();
    master_   = elem.attribute( "master", "-1" ).toInt();
    contains_ = elem.attribute( "contains", QString() );
    absent_   = elem.attribute( "absent", QString() );
    minLaneDbDelta_ = elem.attribute( "minLaneDbDelta", "-1" ).toDouble();
    minMeterDb_ = elem.attribute( "minMeterDb", "1e30" ).toDouble();
    maxMeterDb_ = elem.attribute( "maxMeterDb", "1e30" ).toDouble();
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

// --- mixer-meter-tick -------------------------------------------------------

SApplyResult SMixerMeterTickAction::apply( SProject *project )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "mixer-meter-tick: no main window"; return { false, nullptr }; }
    if( !project ) { qWarning() << "mixer-meter-tick: no project"; return { false, nullptr }; }

    // A monotonically advancing clock, because the ballistics are driven by
    // wall-clock dt and two ticks at the SAME nowMs decay by nothing. Handing
    // the case a default that always moves forward is what keeps a decay
    // assertion (AC3.3) from depending on how fast the box ran the script.
    static qint64 s_fakeNow = 0;
    const qint64 now = nowMs_ >= 0 ? nowMs_ : ( s_fakeNow += 100 );

    const int worked =
        win->mixerMeterTick( arrangement_, position_, now, live_, requestPages_,
                             hidden_ );
    if( worked < 0 ) {
        qWarning() << "mixer-meter-tick FAILED: no pane for arrangement"
                   << arrangement_;
        return { false, nullptr };
    }
    qDebug() << "mixer-meter-tick: OK -" << worked << "strip(s) worked at"
             << (long long) position_ << "live" << live_;
    return { true, nullptr };
}

QStringList SMixerMeterTickAction::knownAttributes() const
{
    return { QStringLiteral( "arrangement" ), QStringLiteral( "position" ),
             QStringLiteral( "live" ), QStringLiteral( "requestPages" ),
             QStringLiteral( "nowMs" ), QStringLiteral( "hidden" ) };
}

void SMixerMeterTickAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "arrangement", arrangement_ );
    elem.setAttribute( "position", (qlonglong) position_ );
    elem.setAttribute( "live", live_ ? "true" : "false" );
    elem.setAttribute( "requestPages", requestPages_ ? "true" : "false" );
    elem.setAttribute( "nowMs", (qlonglong) nowMs_ );
    elem.setAttribute( "hidden", hidden_ );
}

bool SMixerMeterTickAction::readXml( const QDomElement &elem, int )
{
    arrangement_  = elem.attribute( "arrangement", QString() );
    position_     = (offset_t) elem.attribute( "position", "0" ).toLongLong();
    live_         = elem.attribute( "live", "true" ) == QLatin1String( "true" );
    requestPages_ = elem.attribute( "requestPages", "true" ) == QLatin1String( "true" );
    nowMs_        = elem.attribute( "nowMs", "-1" ).toLongLong();
    hidden_       = elem.attribute( "hidden", QString() );
    return true;
}

// --- mixer-strip-set --------------------------------------------------------

SApplyResult SMixerStripSetAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "mixer-strip-set: no main window"; return { false, nullptr }; }
    if( !win->mixerStripSet( track_, control_, gesture_, value_, arrangement_ ) ) {
        qWarning() << "mixer-strip-set FAILED:" << track_ << control_
                   << gesture_ << value_;
        return { false, nullptr };
    }
    qDebug() << "mixer-strip-set: OK -" << track_ << control_ << gesture_ << value_;
    // A GESTURE verb: no undo step of its own. During an open automation pass
    // the control's handler submits NOTHING at all (the recorder takes the
    // value), which is exactly what AC3a.1 asserts by counting undo entries.
    return { true, nullptr };
}

QStringList SMixerStripSetAction::knownAttributes() const
{
    return { QStringLiteral( "track" ), QStringLiteral( "arrangement" ),
             QStringLiteral( "control" ), QStringLiteral( "gesture" ),
             QStringLiteral( "value" ) };
}

void SMixerStripSetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "arrangement", arrangement_ );
    elem.setAttribute( "control", control_ );
    elem.setAttribute( "gesture", gesture_ );
    elem.setAttribute( "value", value_ );
}

bool SMixerStripSetAction::readXml( const QDomElement &elem, int )
{
    track_       = elem.attribute( "track", QString() );
    arrangement_ = elem.attribute( "arrangement", QString() );
    control_     = elem.attribute( "control", "fader" );
    gesture_     = elem.attribute( "gesture", "set" );
    value_       = elem.attribute( "value", "0" ).toDouble();
    return !track_.isEmpty();
}

static const bool s_reg_mixer_meter_tick = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "mixer-meter-tick" ),
        []{ return new SMixerMeterTickAction; } ), true );

static const bool s_reg_mixer_strip_set = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "mixer-strip-set" ),
        []{ return new SMixerStripSetAction; } ), true );
