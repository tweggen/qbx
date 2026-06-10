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
    void onRecordingProgress(double durationSeconds);
    void onRecordingComplete(bool success, const char *error);
    void onStopClicked();
    void updateTimeDisplay();

private:
    QString formatTime(double seconds) const;

    audio::RecordingSession *session_;

    QLabel *statusLabel_ = nullptr;
    QLabel *durationLabel_ = nullptr;
    QLabel *armedTracksLabel_ = nullptr;
    QPushButton *stopButton_ = nullptr;

    double recordedDuration_ = 0.0;
    QTimer *updateTimer_ = nullptr;
    bool isComplete_ = false;
};

#endif
