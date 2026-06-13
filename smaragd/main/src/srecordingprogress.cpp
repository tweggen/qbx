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

    // Setup callbacks from recording session. These fire on the recording WORKER
    // thread, so they must NOT touch widgets/timers directly — doing so corrupts
    // Qt's per-thread state (e.g. "Timers cannot be stopped from another thread"
    // and a SIGSEGV while rewiring the button). Marshal them onto the GUI thread
    // with a queued invocation instead.
    if (session_) {
        session_->onProgress = [this](double durationSeconds) {
            QMetaObject::invokeMethod(this, [this, durationSeconds] {
                this->onRecordingProgress(durationSeconds);
            }, Qt::QueuedConnection);
        };
        session_->onComplete = [this](bool success, const char *error) {
            // `error` points into a std::string owned by the worker thread; copy
            // it now, since the queued slot runs after that string may be gone.
            QString err = error ? QString::fromUtf8(error) : QString();
            QMetaObject::invokeMethod(this, [this, success, err] {
                this->onRecordingComplete(success, err);
            }, Qt::QueuedConnection);
        };
    }

    // Setup timer for time display updates
    updateTimer_ = new QTimer(this);
    connect(updateTimer_, &QTimer::timeout, this, &SRecordingProgressDialog::updateTimeDisplay);
    updateTimer_->start(100);  // Update every 100ms
}

SRecordingProgressDialog::~SRecordingProgressDialog() {
    // Detach from the session so a late worker-thread callback can't invoke into
    // a half-destroyed dialog. (By normal flow the worker has already finished by
    // the time the dialog closes; this is defensive.)
    if (session_) {
        session_->onProgress = nullptr;
        session_->onComplete = nullptr;
    }
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

void SRecordingProgressDialog::onRecordingProgress(double durationSeconds) {
    recordedDuration_ = durationSeconds;
}

void SRecordingProgressDialog::onRecordingComplete(bool success, const QString &error) {
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
    durationLabel_->setText("Duration: " + formatTime(recordedDuration_));
}

void SRecordingProgressDialog::closeEvent(QCloseEvent *event) {
    if (!isComplete_) {
        // Prevent closing while recording is active
        event->ignore();
    } else {
        event->accept();
    }
}
