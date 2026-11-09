#include "app/servicesui/srecordingprogress.h"

#include "app/shell/sapplication.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QCloseEvent>
#include <iomanip>
#include <sstream>

SRecordingProgressDialog::SRecordingProgressDialog(QWidget *parent)
    : QDialog(parent) {
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


    // We deliberately do NOT install session callbacks: those fire on the
    // recording WORKER thread, and a worker std::thread touching Qt (timers,
    // widgets, even posting events) crashed in Qt's per-thread teardown. Instead
    // the timer below polls the session's thread-safe state on the GUI thread.
    //
    // Setup timer for time display + completion polling.
    updateTimer_ = new QTimer(this);
    connect(updateTimer_, &QTimer::timeout, this, &SRecordingProgressDialog::updateTimeDisplay);
    beginTake();
}

void SRecordingProgressDialog::beginTake() {
    isComplete_ = false;
    failed_ = false;
    recordedDuration_ = 0.0;
    updateCount_ = 0;
    statusLabel_->setText("Recording in progress...");
    durationLabel_->setText("Duration: " + formatTime(0.0));
    stopButton_->setText("Stop Recording");
    stopButton_->disconnect();
    connect(stopButton_, &QPushButton::clicked, this, &SRecordingProgressDialog::onStopClicked);
    updateTimer_->start(100);  // Poll every 100ms
}

QString SRecordingProgressDialog::describe() const {
    const char *state = !isComplete_ ? "recording" : (failed_ ? "failed" : "complete");
    return QStringLiteral("open=%1|state=%2|status=%3|duration=%4")
        .arg(isVisible() ? 1 : 0)
        .arg(QLatin1String(state))
        .arg(statusLabel_->text())
        .arg(durationLabel_->text());
}

void SRecordingProgressDialog::pressStop() {
    stopButton_->click();
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
    failed_ = !success;

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
    if (isComplete_) return;
    // Through the APPLICATION, which stops both recorders as one undo step;
    // the audio recorder's own stop() would leave a MIDI half running.
    SApplication &app = SApplication::app();
    if (app.recordingInProgress()) app.stopRecording();
    const QString err = app.recordingError();
    handleCompletion(err.isEmpty(), err);
    accept();
}

void SRecordingProgressDialog::updateTimeDisplay() {
    const SApplication &app = SApplication::app();

    // Poll progress on the GUI thread. The recorders are main-thread-only, so
    // this is a plain read. Only while the take runs: once it has ended the
    // recorders report 0, and the window keeps the length the take reached.
    if (app.recordingInProgress()) {
        recordedDuration_ = app.recordedSeconds();
        durationLabel_->setText("Duration: " + formatTime(recordedDuration_));
    }

    // Refresh the main window's UI every ~500ms to show growing recorded clip (every 5 updates @ 100ms)
    ++updateCount_;
    if (updateCount_ % 5 == 0) {
        if (QWidget *mainWindow = parentWidget()) {
            mainWindow->update();
        }
    }

    // The take may end without this dialog (a punch-out, or the record button
    // pressed again). Follow it.
    if (!isComplete_ && !app.recordingInProgress()) {
        const QString err = app.recordingError();
        handleCompletion(err.isEmpty(), err);
    }
}

void SRecordingProgressDialog::closeEvent(QCloseEvent *event) {
    // Closing the window STOPS the take (it is not modal any more, so refusing
    // to close would leave a window the user cannot get rid of).
    if (!isComplete_ && SApplication::app().recordingInProgress())
        SApplication::app().stopRecording();
    event->accept();
}
