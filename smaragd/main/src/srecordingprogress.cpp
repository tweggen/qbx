#include "srecordingprogress.h"

#include <recording_session.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QCloseEvent>
#include <iomanip>
#include <sstream>

SRecordingProgressDialog::SRecordingProgressDialog(audio::RecordingSession *session,
                                                   QWidget *parent)
    : QDialog(parent), session_(session) {
    setWindowTitle("Recording...");
    setMinimumWidth(400);
    setModal(true);

    // Prevent user from closing the dialog directly
    setWindowFlags(windowFlags() & ~Qt::WindowCloseButtonHint);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Status label
    statusLabel_ = new QLabel("Recording in progress...");
    mainLayout->addWidget(statusLabel_);

    mainLayout->addSpacing(12);

    // Duration label
    durationLabel_ = new QLabel("Duration: 00:00.000");
    mainLayout->addWidget(durationLabel_);

    // Armed tracks info
    armedTracksLabel_ = new QLabel("Armed: (calculating...)");
    mainLayout->addWidget(armedTracksLabel_);

    mainLayout->addSpacing(12);

    // Stop button
    stopButton_ = new QPushButton("Stop Recording");
    mainLayout->addWidget(stopButton_);

    setLayout(mainLayout);

    // Connect button
    connect(stopButton_, &QPushButton::clicked, this, &SRecordingProgressDialog::onStopClicked);

    // We deliberately do NOT install session callbacks: those fire on the
    // recording WORKER thread, and a worker std::thread touching Qt (timers,
    // widgets, even posting events) crashed in Qt's per-thread teardown. Instead
    // the timer below polls the session's thread-safe state on the GUI thread.
    //
    // Setup timer for time display + completion polling.
    updateTimer_ = new QTimer(this);
    connect(updateTimer_, &QTimer::timeout, this, &SRecordingProgressDialog::updateTimeDisplay);
    updateTimer_->start(100);  // Poll every 100ms
}

SRecordingProgressDialog::~SRecordingProgressDialog() {
    if (updateTimer_) {
        updateTimer_->stop();
    }
}

QString SRecordingProgressDialog::formatTime(double seconds) const {
    int minutes = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    int millis = static_cast<int>((seconds - static_cast<int>(seconds)) * 1000);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << secs << "."
        << std::setfill('0') << std::setw(3) << millis;
    return QString::fromStdString(oss.str());
}

void SRecordingProgressDialog::handleCompletion(bool success, const QString &error) {
    updateTimer_->stop();
    isComplete_ = true;

    if (success) {
        statusLabel_->setText("Recording complete!");
        stopButton_->setText("Close");
        stopButton_->disconnect();
        connect(stopButton_, &QPushButton::clicked, this, &QDialog::accept);
    } else {
        statusLabel_->setText(QString("Recording failed: %1").arg(error));
        stopButton_->setText("Close");
        stopButton_->disconnect();
        connect(stopButton_, &QPushButton::clicked, this, &QDialog::reject);
    }
}

void SRecordingProgressDialog::onStopClicked() {
    if (session_ && !isComplete_) {
        session_->requestStop();
    }
}

void SRecordingProgressDialog::updateTimeDisplay() {
    if (!session_) return;

    // Poll progress (thread-safe atomic) on the GUI thread.
    recordedDuration_ = session_->recordedDurationSeconds();
    durationLabel_->setText("Duration: " + formatTime(recordedDuration_));

    // Poll for completion instead of receiving a worker-thread callback.
    if (!isComplete_ && session_->isFinished()) {
        handleCompletion(session_->succeeded(),
                         QString::fromUtf8(session_->errorMessage()));
    }
}

void SRecordingProgressDialog::closeEvent(QCloseEvent *event) {
    if (!isComplete_) {
        // Prevent closing while recording is active
        event->ignore();
    } else {
        event->accept();
    }
}
