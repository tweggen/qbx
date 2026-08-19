#ifndef SOPTIONSDIALOG_H
#define SOPTIONSDIALOG_H

#include <QDialog>

class QTreeWidget;
class QStackedWidget;
class QComboBox;
class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QTimer;
class QLineEdit;

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
    // initialPage indexes the tree/stack pair below (0 = Mouse navigation,
    // 1 = Audio, ...); out-of-range falls back to 0. Lets a caller (e.g. the
    // "audio device unavailable" dialog) open straight to the Audio page
    // instead of making the user find it.
    explicit SOptionsDialog( QWidget *parent = nullptr, int initialPage = 0 );

    // One line per fact the MIDI page is showing, for headless coverage
    // (the house pattern: build the REAL widget off screen, assert on its
    // describe() string). Never parsed by production code.
    QString describeMidiPage() const;
    // Same house pattern, for the Media page (proposal 38 GATE 5b). Reports
    // the backend in use, whether Remember is offered (and why not, when it
    // is not), and one line per account with `password=set|unset|
    // undecryptable` -- NEVER the secret itself, its length or a prefix
    // (AC 15).
    QString describeMediaPage() const;

private slots:
    void apply();             // write all pages to SSettings (no close)
    void accept() override;   // apply + close
    // Opens the DRIVER's own window (proposal 35 Phase 5). BLOCKS until the
    // user closes it — the driver's call is modal — and re-reads the numbers
    // afterwards, because on a driver like the US-16x08 the buffer size lives
    // in that window and nowhere else.
    void onOpenDriverPanel();
    // Runs a LOOPBACK CALIBRATION (proposal 21 L6a): emits a probe on the
    // output, finds it on the input, and OFFERS the residual. It never applies
    // anything by itself — see the .cpp for why that is not timidity.
    void onMeasureLoopback();
    // Media page (proposal 38 GATE 5b). Add/Save both go through
    // onMediaSaveAccount() -- "Add" only clears the form to a fresh accountId
    // first; the persistence call is the SAME one either way, which is what
    // keeps the dialog and the testkit verbs (`set-media-account`) on one
    // code path (AC 13's "refused at the dialog" is this call refusing).
    void onMediaAccountSelected( QListWidgetItem *current, QListWidgetItem *previous );
    void onMediaAddAccount();
    void onMediaSaveAccount();
    void onMediaRemoveAccount();
    void onMediaTestConnection();

private:
    QWidget *buildMousePage();
    QWidget *buildAudioPage();
    QWidget *buildLogPage();
    QWidget *buildMidiPage();
    QWidget *buildPluginsPage();
    QWidget *buildMediaPage();
    void loadMousePage();
    void loadAudioPage();
    void loadLogPage();
    void loadMidiPage();
    void loadPluginsPage();
    void loadMediaPage();
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
    // Media page helpers.
    void refreshMediaAccountList();     // rebuild mediaAccountList_ from the manager
    void loadMediaAccountIntoForm( const QString &accountId );
    void clearMediaForm();

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
    QSpinBox  *recordingOffsetMs_ = nullptr;  // per input DEVICE (proposal 21 L3b)
    // Transport polish (proposal 21 L5): per-USER, not per project.
    QSpinBox  *metronomeLevel_ = nullptr;     // accented click, 0..100 %
    QSpinBox  *countInBars_    = nullptr;
    QSpinBox  *preRollBars_    = nullptr;    // Input device (for recording)
    QComboBox *bufferSizeCombo_;     // Buffer size (ALSA and ASIO)
    // The DRIVER's own settings window (proposal 35 Phase 5). Shown only
    // for a backend that has one — WASAPI does not, because a shared-mode
    // endpoint's settings live in the Windows sound control panel.
    QPushButton *driverPanelBtn_ = nullptr;
    QPushButton *measureLoopbackBtn_ = nullptr;
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

    // Media page (proposal 38 GATE 5b). Save/Remove/Test connection commit
    // IMMEDIATELY through SApplication::mediaAccounts() -- a LIVE action like
    // the Plugins page's "Rescan now", because the state that matters (the
    // account list, the registered SWebDavMediaSource) lives on that
    // long-lived manager, not on this dialog. OK/Cancel/Apply therefore touch
    // nothing extra for this page.
    QListWidget *mediaAccountList_;
    QLineEdit   *mediaAccountId_;    // editable only while adding a NEW account
    QLineEdit   *mediaUrl_;
    QLineEdit   *mediaUser_;
    QLineEdit   *mediaPassword_;     // QLineEdit::Password echo mode
    QCheckBox   *mediaRemember_;
    QLabel      *mediaBackendLabel_;
    QPushButton *mediaAddBtn_;
    QPushButton *mediaSaveBtn_;
    QPushButton *mediaRemoveBtn_;
    QPushButton *mediaTestBtn_;
    QLabel      *mediaTestResultLabel_;
};

#endif // SOPTIONSDIALOG_H
