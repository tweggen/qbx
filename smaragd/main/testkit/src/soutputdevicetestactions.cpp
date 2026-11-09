#include "app/testkit/soutputdevicetestactions.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QWidget>

#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "tw/devices/capture_backend.h"
#include "tw/playback/twspeaker.h"

namespace {

SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    return nullptr;
}

audio::CaptureBackend *captureBackend()
{
    std::shared_ptr<twSpeaker> speaker = SApplication::app().getSpeaker();
    audio::AudioBackend *backend = speaker ? speaker->getBackend() : nullptr;
    return dynamic_cast<audio::CaptureBackend *>( backend );
}

} // namespace

SApplyResult SProbeAudioOutputAction::apply( SProject * )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "probe-audio-output: no main window";
        return { false, nullptr };
    }
    win->checkAudioOutputAtStartup();
    qDebug() << "probe-audio-output: probed";
    return { true, nullptr };
}

QStringList SAssertOutputDeviceAction::knownAttributes() const
{
    return { QStringLiteral( "requestedRate" ), QStringLiteral( "opens" ),
             QStringLiteral( "minOpens" ) };
}

SApplyResult SAssertOutputDeviceAction::apply( SProject * )
{
    audio::CaptureBackend *cap = captureBackend();
    if( !cap ) {
        qWarning() << "assert-output-device: needs the CAPTURE audio backend"
                      " (SMARAGD_AUDIO_BACKEND=capture)";
        return { false, nullptr };
    }
    const qint64 requested = (qint64) cap->lastRequestedRate();
    const qint64 opens     = (qint64) cap->openCount();
    const QString got = QStringLiteral( "requestedRate=%1 opens=%2 rate=%3" )
                            .arg( requested ).arg( opens )
                            .arg( cap->getConfig().sampleRate );
    bool ok = true;
    if( requestedRate_ >= 0 && requested != requestedRate_ ) ok = false;
    if( opens_ >= 0 && opens != opens_ ) ok = false;
    if( minOpens_ >= 0 && opens < minOpens_ ) ok = false;
    if( !ok ) {
        qWarning().noquote() << "assert-output-device FAILED:" << got
                             << "expected requestedRate=" << requestedRate_
                             << "opens=" << opens_ << "minOpens=" << minOpens_;
        return { false, nullptr };
    }
    qDebug().noquote() << "assert-output-device: OK -" << got;
    return { true, nullptr };
}

void SAssertOutputDeviceAction::writeXml( QDomElement &elem ) const
{
    if( requestedRate_ >= 0 ) elem.setAttribute( "requestedRate", requestedRate_ );
    if( opens_ >= 0 )         elem.setAttribute( "opens", opens_ );
    if( minOpens_ >= 0 )      elem.setAttribute( "minOpens", minOpens_ );
}

bool SAssertOutputDeviceAction::readXml( const QDomElement &elem, int )
{
    requestedRate_ = elem.attribute( "requestedRate", "-1" ).toLongLong();
    opens_         = elem.attribute( "opens", "-1" ).toLongLong();
    minOpens_      = elem.attribute( "minOpens", "-1" ).toLongLong();
    return true;
}

static const bool s_reg_probeaudiooutput = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "probe-audio-output" ),
        []{ return new SProbeAudioOutputAction; } ), true );

static const bool s_reg_assertoutputdevice = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-output-device" ),
        []{ return new SAssertOutputDeviceAction; } ), true );
