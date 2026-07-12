#ifndef _SRECORDINGPROGRESS_H
#define _SRECORDINGPROGRESS_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <memory>

class QLabel;
class QPushButton;
class QTimer;

namespace audio {
class RecordingSession;
}

class SRecordingProgressDialog : public QDialog {
    Q_OBJECT

public:
    SRecordingProgressDialog(audio::RecordingSession *session, QWidget *parent = nullptr);
    ~SRecordingProgressDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onStopClicked();
    void updateTimeDisplay();   // GUI-thread timer: polls progress + completion

private:
    QString formatTime(double seconds) const;
    void handleCompletion(bool success, const QString &error);  // on the GUI thread

    audio::RecordingSession *session_;

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
