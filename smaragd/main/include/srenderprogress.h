#ifndef _SRENDERPROGRESS_H
#define _SRENDERPROGRESS_H

#include <QDialog>
#include <QString>
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
    void onRenderProgress(std::size_t written, std::size_t total);
    void onRenderComplete(bool success, const char *error);
    void onCancelClicked();
    void updateTimeDisplay();

private:
    QString formatTime(double seconds) const;

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
};

#endif
