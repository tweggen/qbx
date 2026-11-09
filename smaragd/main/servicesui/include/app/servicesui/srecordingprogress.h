#ifndef _SRECORDINGPROGRESS_H
#define _SRECORDINGPROGRESS_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <memory>

class QLabel;
class QPushButton;
class QTimer;

/**
 * NON-MODAL since proposal 21 L3b (design D7). It used to `exec()` inside the
 * record-button handler, which meant the whole app was blocked for the length
 * of a take: no editing, no transport, no arming a second track, and the
 * growing clip could only be repainted by this dialog poking its parent. Now
 * it is a plain window that POLLS the recording at 10 Hz and shows a
 * duration; closing it stops the take, and stopping the take from anywhere
 * else closes it.
 *
 * IT FOLLOWS THE RECORDING, NOT A RECORDER (QBX-290): SApplication's
 * recordingInProgress() / recordedSeconds() / recordingError(), which answer
 * for both recorders and the preamble. Watching the audio recorder alone
 * reported every MIDI-only take as "Recording failed: no armed tracks" at its
 * first poll, while the take recorded.
 *
 * ONE window for every take: beginTake() resets it for the next press.
 */
class SRecordingProgressDialog : public QDialog {
    Q_OBJECT

public:
    explicit SRecordingProgressDialog(QWidget *parent = nullptr);
    ~SRecordingProgressDialog() override;

    /// A new press: back to "in progress", the poll running again.
    void beginTake();
    /// Test seam: `open=<0|1>|state=<recording|complete|failed>|status=<the
    /// status line>|duration=<the duration line>`, read off the widgets.
    /// Fields are APPENDED, never inserted.
    QString describe() const;
    /// Test seam: the Stop button, clicked as a hand clicks it.
    void pressStop();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStopClicked();
    void updateTimeDisplay();   // GUI-thread timer: polls progress + completion

private:
    QString formatTime(double seconds) const;
    void handleCompletion(bool success, const QString &error);  // on the GUI thread

    QLabel *statusLabel_ = nullptr;
    QLabel *durationLabel_ = nullptr;
    QLabel *armedTracksLabel_ = nullptr;
    QPushButton *stopButton_ = nullptr;

    double recordedDuration_ = 0.0;
    QTimer *updateTimer_ = nullptr;
    bool isComplete_ = false;
    bool failed_ = false;
    int updateCount_ = 0;  // Counter to trigger UI refresh every 500ms
};

#endif
