#ifndef SOPTIONSDIALOG_H
#define SOPTIONSDIALOG_H

#include <QDialog>

class QTreeWidget;
class QStackedWidget;
class QComboBox;
class QCheckBox;
class QLabel;
class QSpinBox;

// Per-user preferences dialog: a category tree on the left, one option page per
// category on the right (QStackedWidget), with OK / Cancel / Apply. Reads from
// and writes to SSettings. Categories: Mouse navigation, Audio, Log.
class SOptionsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SOptionsDialog( QWidget *parent = nullptr );

private slots:
    void apply();             // write all pages to SSettings (no close)
    void accept() override;   // apply + close

private:
    QWidget *buildMousePage();
    QWidget *buildAudioPage();
    QWidget *buildLogPage();
    void loadMousePage();
    void loadAudioPage();
    void loadLogPage();
    void applyMousePage();
    void applyAudioPage();
    void applyLogPage();

    QTreeWidget    *tree_;
    QStackedWidget *stack_;

    // Mouse-navigation page.
    QComboBox *wheelPlain_;
    QComboBox *wheelShift_;
    QComboBox *wheelCtrl_;
    QComboBox *wheelCtrlShift_;
    QCheckBox *zoomToCursor_;
    QCheckBox *invertZoom_;

    // Audio page.
    QComboBox *audioDevice_;         // Output device
    QComboBox *audioInputDevice_;    // Input device (for recording)
    QComboBox *bufferSizeCombo_;     // Buffer size (ALSA only)
    QLabel *outputLatencyLabel_;     // Output latency display
    QLabel *inputLatencyLabel_;      // Input latency display

    // Log page (proposal 24). Console and level take effect immediately on
    // Apply; capacity resizes the ring, which necessarily discards its buffer.
    QCheckBox *logConsole_;
    QComboBox *logLevel_;
    QSpinBox  *logCapacity_;
    QCheckBox *logToFile_;
    QLabel    *logPathLabel_;
};

#endif // SOPTIONSDIALOG_H
