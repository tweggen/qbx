#ifndef SOPTIONSDIALOG_H
#define SOPTIONSDIALOG_H

#include <QDialog>

class QTreeWidget;
class QStackedWidget;
class QComboBox;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;

// Per-user preferences dialog: a category tree on the left, one option page per
// category on the right (QStackedWidget), with OK / Cancel / Apply. Reads from
// and writes to SSettings. Categories: Mouse navigation, Audio, Log, Plugins.
//
// The tree item and the stack page for a category MUST be added in the same
// order: the mapping is by top-level index, nothing else.
//
// The MIDI page (proposal 37 P7b) mirrors the Audio one deliberately: the same
// build/load/apply triple, the same "the device list comes from the backend"
// rule. It asks SApplication's MIDI-out pump for the port lists rather than
// minting a MidiOutput of its own - the capture backend registers the most
// recently constructed instance as the one the testkit reads, so a throwaway
// port here would unregister the live one when it died.
class SOptionsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SOptionsDialog( QWidget *parent = nullptr );

    // One line per fact the MIDI page is showing, for headless coverage
    // (the house pattern: build the REAL widget off screen, assert on its
    // describe() string). Never parsed by production code.
    QString describeMidiPage() const;

private slots:
    void apply();             // write all pages to SSettings (no close)
    void accept() override;   // apply + close

private:
    QWidget *buildMousePage();
    QWidget *buildAudioPage();
    QWidget *buildLogPage();
    QWidget *buildMidiPage();
    QWidget *buildPluginsPage();
    void loadMousePage();
    void loadAudioPage();
    void loadLogPage();
    void loadMidiPage();
    void loadPluginsPage();
    void applyMousePage();
    void applyAudioPage();
    void applyLogPage();
    void applyMidiPage();
    void applyPluginsPage();
    // Plugins page helpers.
    void addPluginDir();
    void removePluginDirs();
    void resetPluginDirsToDefaults();
    void rescanPluginsNow();
    void updatePluginScanStatus();

    QTreeWidget    *tree_;
    QStackedWidget *stack_;

    // Mouse-navigation page.
    QComboBox *wheelPlain_;
    QComboBox *wheelShift_;
    QComboBox *wheelCtrl_;
    QComboBox *wheelCtrlShift_;
    // One percentage scaling all four wheel gestures (SOpt::WheelSensitivityPct).
    QSpinBox  *wheelSensitivity_;
    QCheckBox *zoomToCursor_;
    QCheckBox *invertZoom_;
    QCheckBox *followPlayhead_;

    // Audio page.
    QComboBox *audioDevice_;         // Output device
    QComboBox *audioInputDevice_;
    QSpinBox  *recordingOffsetMs_ = nullptr;  // per input DEVICE (proposal 21 L3b)    // Input device (for recording)
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

    // MIDI page (proposal 37 P7b). `midiVirtualBtn_` is DISABLED where the
    // backend has no virtual-port concept (WinMM - tw/devices CONTRACT inv.
    // 18), the same "gate the offer on the capability, not on the platform"
    // shape the MP3 render option uses.
    QListWidget *midiOutputs_;
    QListWidget *midiInputs_;
    QSpinBox    *midiOffsetMs_;
    QCheckBox   *midiChaseNoteOns_;
    QPushButton *midiVirtualBtn_;
    QLabel      *midiStatusLabel_;

    // Plugins page (proposal 08 M2). "Rescan now" is a LIVE action, like the
    // log page's setConsole: it applies the directory list and kicks off a
    // background scan without waiting for OK. pluginStatusTimer_ polls the
    // engine scanner — the scan thread must not emit Qt signals.
    QListWidget *pluginDirs_;
    QPushButton *pluginRemoveBtn_;
    QPushButton *pluginRescanBtn_;
    QCheckBox   *pluginScanOnStartup_;
    QLabel      *pluginStatusLabel_;
    QTimer      *pluginStatusTimer_;
};

#endif // SOPTIONSDIALOG_H
