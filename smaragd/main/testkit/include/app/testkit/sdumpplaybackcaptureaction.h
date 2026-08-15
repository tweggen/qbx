#ifndef _SDUMPPLAYBACKCAPTUREACTION_H
#define _SDUMPPLAYBACKCAPTUREACTION_H

#include "app/actions/saction.h"
#include <QString>
#include <QStringList>

// dump-playback-capture: write what the CAPTURE backend recorded during
// playback into the test output directory as a 16-bit PCM WAV.
//
//   <dump-playback-capture filename="playback.wav"/>
//
// The file lands beside the renders (assert-audio-* and assert-source-position
// resolve their filename against the same directory), so a captured playback is
// assertable with exactly the verbs a render is — which is the point: those
// verbs, and the position decoder in particular, were until now only ever
// pointed at RenderSession's output.
//
// Rejects unless the active backend is the capture backend, naming the one it
// found. A silent pass there would be the worst outcome available: the case
// would go on to assert over a stale or absent file.
class SDumpPlaybackCaptureAction : public SAction {
public:
    SDumpPlaybackCaptureAction() = default;
    explicit SDumpPlaybackCaptureAction(const QString &filename)
        : filename_(filename) {}

    QString name() const override { return QStringLiteral("dump-playback-capture"); }
    QStringList knownAttributes() const override;
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    QString filename() const { return filename_; }

private:
    QString filename_;
    // Minimum frames the recording must contain to be worth writing. A case
    // that captured nothing has a broken transport, not a short file, and the
    // decode assertions that follow would blame the wrong thing.
    qint64  minFrames_ = 0;
};

#endif
