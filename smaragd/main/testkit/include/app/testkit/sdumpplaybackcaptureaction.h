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
//
// CAPTURED FRAME 0 IS THE DEVICE START, NOT THE LOCATOR POSITION (QBX-222).
// It is the locator position playback began from PLUS whatever leading silence
// the box inserted. twSpeaker::monitorReadaheadBuffer starts the backend once
// the readahead reports ready, but that test is weaker than "the page at the
// start position is frozen and valid"; when the first callback beats it,
// twAudioEngine's page lookup takes its graceful-degradation branch and emits
// ONE BLOCK OF SILENCE -- and leaves no trace in the log, because the FOUND and
// UNDERRUN messages share one rate-limited counter (% 500 and % 50).
//
// So the number of leading silent frames is a WALL-CLOCK QUANTITY OF THE
// MACHINE. assert-metronome-clicks already says so for its grid ("anchored on
// the FIRST detected onset, deliberately") and uses 1024 frames, one device
// block, as the house tolerance. Anything measured over this file must do the
// same: bound the start with assert-audio-onset, and begin the measurement
// window one device block in. A window starting at frame 0 with a tight band is
// the shape that cost this project two unexplained flakes two months apart --
// RMS 0.4823 against [0.5401, 0.5735] on a 4096-frame window, and 0.272503
// against [0.350061, 0.427853] on a 2048-frame one, both of them exactly
// "golden * sqrt((N - 1024) / N)" and neither of them a product defect.
//
// And do NOT assert this file's FRAME COUNT. A case that stops on wait-playhead
// gets "however many blocks were pumped before the stop took effect" -- 47
// blocks locally and 48 on a CI runner for the same case. The count is an
// excellent triage signal (a one-block difference identifies the slip in
// seconds) and a flaky assertion.
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
    // decode assertions that follow would blame the wrong thing. It is checked
    // against the REAL recording, before any injection below.
    qint64  minFrames_ = 0;
    // TEST-ONLY FAULT INJECTOR (QBX-222). Prepend this many frames of silence
    // to the file, simulating the leading silent block the engine emits when
    // the first callback beats the page it needs. It exists so a case can GATE
    // its own robustness against the hazard described above, instead of
    // discovering it as an unexplained flake on a loaded CI runner months
    // later - which is what happened twice. A non-zero value logs a WARNING
    // naming the injection, so a doctored recording can never be read as a
    // real one.
    qint64  leadSilenceFrames_ = 0;
};

#endif
