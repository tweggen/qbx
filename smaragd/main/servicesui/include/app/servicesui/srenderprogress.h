#ifndef _SRENDERPROGRESS_H
#define _SRENDERPROGRESS_H

#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <cstdint>
#include <memory>

class QProgressBar;
class QLabel;
class QPushButton;
class QTimer;

namespace audio {
class RenderSession;
}

class SRenderProgressDialog : public QDialog {
    Q_OBJECT

public:
    SRenderProgressDialog(audio::RenderSession *session, const QString &filePath,
                         QWidget *parent = nullptr);
    ~SRenderProgressDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onCancelClicked();
    void updateTimeDisplay();

private:
    QString formatTime(double seconds) const;
    void finish(bool success, const QString &error);

    audio::RenderSession *session_;
    QString filePath_;

    QLabel *filePathLabel_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QLabel *progressTextLabel_ = nullptr;
    QLabel *timeLabel_ = nullptr;
    QLabel *estimatedTimeLabel_ = nullptr;
    QPushButton *cancelButton_ = nullptr;

    std::uint32_t sampleRate_ = 48000;
    QTimer *updateTimer_ = nullptr;
    QElapsedTimer elapsed_;     // wall clock since the dialog opened
};

#endif
