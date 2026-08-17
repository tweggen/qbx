#include "app/servicesui/srecordingprogress.h"

#include "app/shell/saudiorecorder.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QCloseEvent>
#include <iomanip>
#include <sstream>

SRecordingProgressDialog::SRecordingProgressDialog(SAudioRecorder *recorder,
                                                   QWidget *parent)
    : QDialog(parent), recorder_(recorder) {
    setWindowTitle("Recording...");
    setMinimumWidth(400);
    // NON-MODAL (proposal 21 L3b): the take runs while the app stays usable.
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);

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
    if (recorder_ && !isComplete_) {
        recorder_->stop();
        handleCompletion(true, QString());
        accept();
    }
}

void SRecordingProgressDialog::updateTimeDisplay() {
    if (!recorder_) return;

    // Poll progress on the GUI thread. The recorder is main-thread-only, so
    // this is a plain read.
    recordedDuration_ = recorder_->recordedSeconds();
    durationLabel_->setText("Duration: " + formatTime(recordedDuration_));

    // Refresh the main window's UI every ~500ms to show growing recorded clip (every 5 updates @ 100ms)
    ++updateCount_;
    if (updateCount_ % 5 == 0) {
        if (QWidget *mainWindow = parentWidget()) {
            mainWindow->update();
        }
    }

    // The take may end without this dialog (a punch-out, or the record button
    // pressed again). Follow it.
    if (!isComplete_ && !recorder_->isActive()) {
        const QString err = recorder_->errorMessage();
        handleCompletion(err.isEmpty(), err);
    }
}

void SRecordingProgressDialog::closeEvent(QCloseEvent *event) {
    // Closing the window STOPS the take (it is not modal any more, so refusing
    // to close would leave a window the user cannot get rid of).
    if (!isComplete_ && recorder_ && recorder_->isActive()) recorder_->stop();
    event->accept();
}
