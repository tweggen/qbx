#ifndef _SRECORDINGPROGRESS_H
#define _SRECORDINGPROGRESS_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <memory>

class QLabel;
class QPushButton;
class QTimer;

class SAudioRecorder;

/**
 * NON-MODAL since proposal 21 L3b (design D7). It used to `exec()` inside the
 * record-button handler, which meant the whole app was blocked for the length
 * of a take: no editing, no transport, no arming a second track, and the
 * growing clip could only be repainted by this dialog poking its parent. Now
 * it is a plain window that POLLS `SAudioRecorder` at 10 Hz and shows a
 * duration; closing it stops the take, and stopping the take from anywhere
 * else closes it.
 */
class SRecordingProgressDialog : public QDialog {
    Q_OBJECT

public:
    SRecordingProgressDialog(SAudioRecorder *recorder, QWidget *parent = nullptr);
    ~SRecordingProgressDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStopClicked();
    void updateTimeDisplay();   // GUI-thread timer: polls progress + completion

private:
    QString formatTime(double seconds) const;
    void handleCompletion(bool success, const QString &error);  // on the GUI thread

    SAudioRecorder *recorder_;

    QLabel *statusLabel_ = nullptr;
    QLabel *durationLabel_ = nullptr;
    QLabel *armedTracksLabel_ = nullptr;
    QPushButton *stopButton_ = nullptr;

    double recordedDuration_ = 0.0;
    QTimer *updateTimer_ = nullptr;
    bool isComplete_ = false;
    int updateCount_ = 0;  // Counter to trigger UI refresh every 500ms
};

#endif
