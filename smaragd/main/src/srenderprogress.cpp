#include "srenderprogress.h"

#include <render_session.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QCloseEvent>
#include <cmath>
#include <iomanip>
#include <sstream>

SRenderProgressDialog::SRenderProgressDialog(audio::RenderSession *session,
                                             const QString &filePath, QWidget *parent)
    : QDialog(parent), session_(session), filePath_(filePath) {
    setWindowTitle("Rendering...");
    setMinimumWidth(500);
    setModal(true);

    // Prevent user from closing the dialog directly
    setWindowFlags(windowFlags() & ~Qt::WindowCloseButtonHint);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // File path label
    filePathLabel_ = new QLabel("Writing to: " + filePath_);
    mainLayout->addWidget(filePathLabel_);

    mainLayout->addSpacing(12);

    // Progress bar
    progressBar_ = new QProgressBar();
    progressBar_->setMinimum(0);
    progressBar_->setMaximum(100);
    progressBar_->setValue(0);
    mainLayout->addWidget(progressBar_);

    // Progress text (percentage)
    progressTextLabel_ = new QLabel("0% (0 / 0 samples)");
    mainLayout->addWidget(progressTextLabel_);

    mainLayout->addSpacing(12);

    // Time display
    timeLabel_ = new QLabel("Elapsed: 00:00 / Remaining: --:--");
    mainLayout->addWidget(timeLabel_);

    estimatedTimeLabel_ = new QLabel("");
    mainLayout->addWidget(estimatedTimeLabel_);

    mainLayout->addSpacing(12);

    // Cancel button
    cancelButton_ = new QPushButton("Cancel");
    mainLayout->addWidget(cancelButton_);

    setLayout(mainLayout);

    // Connect signals
    connect(cancelButton_, &QPushButton::clicked, this, &SRenderProgressDialog::onCancelClicked);

    // Setup timer for time display updates
    updateTimer_ = new QTimer(this);
    connect(updateTimer_, &QTimer::timeout, this, &SRenderProgressDialog::updateTimeDisplay);
    updateTimer_->start(100);  // Update every 100ms
}

SRenderProgressDialog::~SRenderProgressDialog() {
    if (updateTimer_) {
        updateTimer_->stop();
    }
}

void SRenderProgressDialog::onRenderProgress(std::size_t written, std::size_t total) {
    if (total > 0) {
        int percentage = static_cast<int>((written * 100) / total);
        progressBar_->setValue(percentage);

        QString progressText =
            QString::asprintf("%d%% (%zu / %zu samples)", percentage, written, total);
        progressTextLabel_->setText(progressText);
    }
}

void SRenderProgressDialog::onRenderComplete(bool success, const char *error) {
    updateTimer_->stop();

    if (success) {
        progressBar_->setValue(100);
        progressTextLabel_->setText("100% - Render complete!");
        cancelButton_->setText("Close");
        cancelButton_->setEnabled(true);
    } else {
        cancelButton_->setText("Close");
        cancelButton_->setEnabled(true);
        QString errorMsg = error ? QString::fromStdString(std::string(error)) : "Unknown error";
        estimatedTimeLabel_->setText("Error: " + errorMsg);
    }
}

void SRenderProgressDialog::onCancelClicked() {
    if (session_) {
        session_->requestCancel();
    }
    accept();
}

void SRenderProgressDialog::updateTimeDisplay() {
    if (!session_) return;

    std::size_t written = session_->samplesWritten();
    std::size_t total = session_->totalSamples();

    // Update progress bar and text
    if (total > 0) {
        int percentage = static_cast<int>((100.0 * written) / total);
        progressBar_->setValue(percentage);
        progressTextLabel_->setText(
            QString("%1% (%2 / %3 samples)")
                .arg(percentage)
                .arg(written)
                .arg(total));

        // Calculate elapsed time and speed
        double elapsedSeconds = static_cast<double>(written) / sampleRate_;

        if (elapsedSeconds > 0) {
            // Calculate rendering rate (samples per second)
            double rate = static_cast<double>(written) / elapsedSeconds;

            if (rate > 0) {
                // Calculate remaining time
                double remainingSeconds = static_cast<double>(total - written) / rate;

                QString elapsedStr = formatTime(elapsedSeconds);
                QString remainingStr = formatTime(remainingSeconds);
                QString timeText =
                    QString("Elapsed: %1 / Remaining: %2").arg(elapsedStr, remainingStr);
                timeLabel_->setText(timeText);

                // Rendering speed indicator
                double realtimeRatio = rate / sampleRate_;
                QString speedText =
                    QString("Speed: %.1fx realtime").arg(realtimeRatio);
                estimatedTimeLabel_->setText(speedText);
            }
        }
    }
}

QString SRenderProgressDialog::formatTime(double seconds) const {
    int mins = static_cast<int>(seconds / 60.0);
    int secs = static_cast<int>(seconds) % 60;
    return QString::asprintf("%02d:%02d", mins, secs);
}

void SRenderProgressDialog::closeEvent(QCloseEvent *event) {
    // Prevent closing during render
    if (session_ && session_->isRunning()) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}
