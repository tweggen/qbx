#ifndef SOUTPUTDEVICETESTACTIONS_H
#define SOUTPUTDEVICETESTACTIONS_H

#include "app/actions/saction.h"

/**
 * QBX-296: which sample rate qbx ASKS the output device for.
 *
 * On a Mac (CoreAudio) and on ASIO a requested rate is an instruction: the
 * backend SETS the device to it, for every application on that device. So
 * "the startup probe asks for no rate" and "a project that keeps the device's
 * rate asks for none" are claims about the REQUEST, which the opened rate
 * cannot show: the capture backend keeps whatever rate it had when asked for
 * none. Both verbs need SMARAGD_AUDIO_BACKEND=capture.
 *
 * <probe-audio-output/>
 *   Runs SMainWindow::checkAudioOutputAtStartup(), the call main.cpp makes
 *   once the window is up in an interactive start (unreachable from a
 *   --test-case run, which is why this verb exists).
 *
 * <assert-output-device requestedRate="0" opens="1" minOpens="1"/>
 *   requestedRate: the preferredRate of the latest open (0 = none);
 *   opens / minOpens: how many opens the backend has seen. Each is optional.
 */
class SProbeAudioOutputAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "probe-audio-output" ); }
    QStringList knownAttributes() const override { return {}; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement & ) const override {}
    bool readXml( const QDomElement &, int ) override { return true; }
};

class SAssertOutputDeviceAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-output-device" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    qint64 requestedRate_ = -1;
    qint64 opens_ = -1;
    qint64 minOpens_ = -1;
};

#endif // SOUTPUTDEVICETESTACTIONS_H
