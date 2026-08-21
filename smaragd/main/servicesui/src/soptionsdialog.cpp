#include "app/servicesui/soptionsdialog.h"
#include "app/servicesui/soptions.h"
#include "app/shell/ssettings.h"
#include "app/shell/sapplication.h"
#include "tw/playback/twspeaker.h"
#include "tw/devices/audio_backend.h"
#include "tw/record/loopback_runner.h"
#include "tw/devices/audio_input.h"
#include "tw/devices/midi_output.h"
#include "app/shell/smidiinputhub.h"
#include "app/shell/smidioutpump.h"
#include "app/shell/smediaaccountmanager.h"
#include "tw/core/twlog.h"

#include <QTreeWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QAbstractItemView>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>

#include "tw/plugins/twpluginsearchpaths.h"

// A combo populated with every wheel action; the enum value is the item data.
static QComboBox *makeWheelActionCombo()
{
    QComboBox *c = new QComboBox;
    for( int a = SOpt::None; a <= SOpt::ZoomVertical; ++a ) {
        c->addItem( SOpt::wheelActionLabel( (SOpt::WheelAction) a ), a );
    }
    return c;
}

static void selectByData( QComboBox *c, const QVariant &data )
{
    int i = c->findData( data );
    if( i >= 0 ) c->setCurrentIndex( i );
}

SOptionsDialog::SOptionsDialog( QWidget *parent, int initialPage )
    : QDialog( parent )
{
    setWindowTitle( "Options" );

    tree_ = new QTreeWidget;
    tree_->setHeaderHidden( true );
    tree_->setFixedWidth( 160 );
    // The tree item order and the stack order must MATCH: currentItemChanged
    // maps by top-level index, nothing else.
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Mouse navigation" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Event Editor" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Audio" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "MIDI" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Log" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Plugins" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Media" ) ) );

    stack_ = new QStackedWidget;
    stack_->addWidget( buildMousePage() );        // index 0
    stack_->addWidget( buildEventEditorPage() );  // index 1
    stack_->addWidget( buildAudioPage() );        // index 2
    stack_->addWidget( buildMidiPage() );         // index 3
    stack_->addWidget( buildLogPage() );          // index 4
    stack_->addWidget( buildPluginsPage() );      // index 5
    stack_->addWidget( buildMediaPage() );        // index 6

    QObject::connect( tree_, &QTreeWidget::currentItemChanged,
                      this, [this]( QTreeWidgetItem *cur, QTreeWidgetItem * ) {
        if( cur ) stack_->setCurrentIndex( tree_->indexOfTopLevelItem( cur ) );
    } );

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply );
    QObject::connect( buttons, &QDialogButtonBox::accepted, this, &SOptionsDialog::accept );
    QObject::connect( buttons, &QDialogButtonBox::rejected, this, &SOptionsDialog::reject );
    QObject::connect( buttons->button( QDialogButtonBox::Apply ),
                      &QPushButton::clicked, this, &SOptionsDialog::apply );

    QHBoxLayout *top = new QHBoxLayout;
    top->addWidget( tree_ );
    top->addWidget( stack_, 1 );

    QVBoxLayout *root = new QVBoxLayout( this );
    root->addLayout( top, 1 );
    root->addWidget( buttons );

    // Populate from current settings.
    loadMousePage();
    loadEventEditorPage();
    loadAudioPage();
    loadMidiPage();

    // -1 is the "no explicit page" sentinel (AC-b1): resolve it to the page
    // this dialog was last CLOSED on, by NAME (see SOpt::OptionsLastPage's
    // comment for why a name and not the index this used to be). Anything
    // else -- no remembered name yet, a name no page currently carries, or
    // an explicit out-of-range index -- falls back to page 0, exactly as an
    // out-of-range explicit page always has.
    if( initialPage < 0 ) {
        const QString rememberedName = SSettings::instance().value(
            SOpt::OptionsLastPage, SOpt::def( SOpt::OptionsLastPage ) ).toString();
        initialPage = 0;
        for( int i = 0; i < tree_->topLevelItemCount(); ++i ) {
            if( tree_->topLevelItem( i )->text( 0 ) == rememberedName ) {
                initialPage = i;
                break;
            }
        }
    } else if( initialPage >= tree_->topLevelItemCount() ) {
        initialPage = 0;
    }
    tree_->setCurrentItem( tree_->topLevelItem( initialPage ) );
    resize( 520, 320 );
}

QWidget *SOptionsDialog::buildMousePage()
{
    QWidget *page = new QWidget;
    QFormLayout *form = new QFormLayout( page );

    wheelPlain_     = makeWheelActionCombo();
    wheelShift_     = makeWheelActionCombo();
    wheelCtrl_      = makeWheelActionCombo();
    wheelCtrlShift_ = makeWheelActionCombo();
    form->addRow( "Wheel:", wheelPlain_ );
    form->addRow( "Shift + Wheel:", wheelShift_ );
    form->addRow( "Ctrl + Wheel:", wheelCtrl_ );
    form->addRow( "Ctrl + Shift + Wheel:", wheelCtrlShift_ );

    wheelSensitivity_ = new QSpinBox;
    // Percent, not a raw factor: "150 %" is self-explanatory where "1.5" needs a
    // legend. The floor is 10 % rather than 0 because a zero would stall every
    // gesture and read as a broken wheel; the arranger clamps to this same range
    // in case the INI was hand-edited.
    wheelSensitivity_->setRange( 10, 500 );
    wheelSensitivity_->setSingleStep( 10 );
    wheelSensitivity_->setSuffix( " %" );
    wheelSensitivity_->setToolTip(
        "How far one wheel notch travels, for all four gestures at once. "
        "Lower = less sensitive (more wheel per step), higher = more sensitive. "
        "100 % is the shipped feel." );
    form->addRow( "Wheel sensitivity:", wheelSensitivity_ );
    form->addRow( QString(), new QLabel( "Lower = less sensitive, higher = more "
                                         "sensitive. 100 % = default." ) );

    zoomToCursor_   = new QCheckBox( "Zoom toward the mouse cursor" );
    invertZoom_     = new QCheckBox( "Invert zoom direction" );
    followPlayhead_ = new QCheckBox( "Follow the playhead during playback" );
    form->addRow( QString(), zoomToCursor_ );
    form->addRow( QString(), invertZoom_ );
    form->addRow( QString(), followPlayhead_ );

    return page;
}

// ------------------------------------------------------------ Event Editor

// The piano roll's own copy of the mouse page above (same four gestures, same
// sensitivity/zoom-to-cursor/invert-zoom controls, own SOpt namespace so the
// two views can be tuned independently — SOpt::EventWheelPlain and co.). No
// "Follow the playhead" here: the event editor has no playhead-follow feature
// of its own to mirror (its view instead LINKS to the arranger's zoom/scroll,
// toggled from the dock's own "Link" checkbox, not from Options).
QWidget *SOptionsDialog::buildEventEditorPage()
{
    QWidget *page = new QWidget;
    QFormLayout *form = new QFormLayout( page );

    form->addRow( new QLabel( "Mouse wheel over the piano roll (note grid, "
                              "velocity and CC lanes):" ) );

    eventWheelPlain_     = makeWheelActionCombo();
    eventWheelShift_     = makeWheelActionCombo();
    eventWheelCtrl_      = makeWheelActionCombo();
    eventWheelCtrlShift_ = makeWheelActionCombo();
    form->addRow( "Wheel:", eventWheelPlain_ );
    form->addRow( "Shift + Wheel:", eventWheelShift_ );
    form->addRow( "Ctrl + Wheel:", eventWheelCtrl_ );
    form->addRow( "Ctrl + Shift + Wheel:", eventWheelCtrlShift_ );

    eventWheelSensitivity_ = new QSpinBox;
    eventWheelSensitivity_->setRange( 10, 500 );
    eventWheelSensitivity_->setSingleStep( 10 );
    eventWheelSensitivity_->setSuffix( " %" );
    eventWheelSensitivity_->setToolTip(
        "How far one wheel notch travels in the piano roll, for all four "
        "gestures at once. 100 % is the shipped feel (the same as the "
        "arranger's default)." );
    form->addRow( "Wheel sensitivity:", eventWheelSensitivity_ );
    form->addRow( QString(), new QLabel( "Lower = less sensitive, higher = more "
                                         "sensitive. 100 % = default." ) );

    eventZoomToCursor_ = new QCheckBox( "Zoom toward the mouse cursor" );
    eventInvertZoom_   = new QCheckBox( "Invert zoom direction" );
    form->addRow( QString(), eventZoomToCursor_ );
    form->addRow( QString(), eventInvertZoom_ );

    return page;
}

QWidget *SOptionsDialog::buildAudioPage()
{
    QWidget *page = new QWidget;
    QFormLayout *form = new QFormLayout( page );

    audioDevice_ = new QComboBox;
    form->addRow( "Output device:", audioDevice_ );
    form->addRow( new QLabel( "Device changes take effect on the next Play." ) );

    outputLatencyLabel_ = new QLabel;
    form->addRow( "Output latency:", outputLatencyLabel_ );

    audioInputDevice_ = new QComboBox;
    form->addRow( "Input device:", audioInputDevice_ );
    form->addRow( new QLabel( "Device selection is used when recording starts." ) );
    connect( audioInputDevice_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &SOptionsDialog::onAudioInputDeviceSelected );

    inputLatencyLabel_ = new QLabel;
    form->addRow( "Input latency:", inputLatencyLabel_ );

    // THE LAST TERM OF THE PLACEMENT CONVERSION (proposal 21 L3b, design D6).
    // Per INPUT DEVICE, and stored under the device's NAME, so it survives an
    // id change the way midiPortId() does. Same range and same sign as the
    // MIDI-out offset on the MIDI page: POSITIVE = the driver under-reports,
    // compensate more, place the recorded audio EARLIER. It is the number a
    // "record a click, look at where it landed, type the difference"
    // calibration produces.
    recordingOffsetMs_ = new QSpinBox;
    recordingOffsetMs_->setRange( -500, 500 );
    recordingOffsetMs_->setSuffix( " ms" );
    form->addRow( "Recording offset (+ = earlier):", recordingOffsetMs_ );
    form->addRow( new QLabel(
        "Applies to the selected input device. The driver's reported latencies "
        "are compensated automatically; this corrects what it misreports." ) );

    // MEASURE IT INSTEAD OF TYPING IT (proposal 21 L6a). The label above asks
    // the user to produce a number by ear; this produces it with a cable.
    measureLoopbackBtn_ = new QPushButton( "Measure with a loopback cable..." );
    measureLoopbackBtn_->setToolTip(
        "Plays a short click out and listens for it coming back, then offers the "
        "difference between the real round trip and what the driver claims.\n\n"
        "Needs a cable from an output to the input, and ONE device selected for "
        "both directions." );
    form->addRow( QString(), measureLoopbackBtn_ );
    connect( measureLoopbackBtn_, &QPushButton::clicked,
             this, &SOptionsDialog::onMeasureLoopback );

    // TRANSPORT POLISH (proposal 21 L5). Three per-USER knobs, on the Audio
    // page because that is where the transport's other timing numbers already
    // are. Whether the metronome is ON is a PROJECT property and stays on the
    // transport bar - this is only how loud it is.
    metronomeLevel_ = new QSpinBox;
    metronomeLevel_->setRange( 0, 100 );
    metronomeLevel_->setSuffix( " %" );
    form->addRow( "Metronome level:", metronomeLevel_ );

    countInBars_ = new QSpinBox;
    countInBars_->setRange( 0, 8 );
    countInBars_->setSuffix( " bar(s)" );
    form->addRow( "Count-in:", countInBars_ );

    preRollBars_ = new QSpinBox;
    preRollBars_->setRange( 0, 8 );
    preRollBars_->setSuffix( " bar(s)" );
    form->addRow( "Pre-roll:", preRollBars_ );
    form->addRow( new QLabel(
        "Count-in clicks the bars BEFORE the record position without moving "
        "the playhead; pre-roll ROLLS them. Recording begins at the locator "
        "either way." ) );

    bufferSizeCombo_ = new QComboBox;
    form->addRow( "Buffer size:", bufferSizeCombo_ );
    form->addRow( new QLabel( "Smaller buffer = lower latency but higher CPU load. "
                             "Requires restart of playback to take effect." ) );

    // THE DRIVER'S OWN PANEL (proposal 35 Phase 5). Hidden unless the current
    // backend has one, which today means ASIO: a WASAPI endpoint's settings
    // live in the Windows sound control panel and are not ours to open.
    //
    // It matters more than it looks. On a driver whose buffer size is FIXED —
    // the gate hardware reports min == max == preferred == 256 — the combo
    // above has exactly one entry, and this window is the ONLY place that
    // number can be changed at all.
    driverPanelBtn_ = new QPushButton( "Driver Control Panel..." );
    driverPanelBtn_->setToolTip(
        "Opens the audio driver's own settings window. Buffer size and sample "
        "rate changed there take effect on the next Play." );
    form->addRow( QString(), driverPanelBtn_ );
    connect( driverPanelBtn_, &QPushButton::clicked,
             this, &SOptionsDialog::onOpenDriverPanel );

    return page;
}

// ---------------------------------------------------------------- MIDI page

QWidget *SOptionsDialog::buildMidiPage()
{
    QWidget *page = new QWidget;
    QVBoxLayout *box = new QVBoxLayout( page );

    midiStatusLabel_ = new QLabel;
    box->addWidget( midiStatusLabel_ );

    box->addWidget( new QLabel( "Output ports:" ) );
    midiOutputs_ = new QListWidget;
    // Informational, not a selection: a TRACK chooses its port, by the
    // portable NAME shown here (set-track-midi-output port=...). The
    // machine-local id each name resolves to lives in smaragd.ini, keyed by
    // the name, so the same project follows a different device on the next
    // machine without editing the project.
    midiOutputs_->setSelectionMode( QAbstractItemView::NoSelection );
    box->addWidget( midiOutputs_, 1 );

    midiVirtualBtn_ = new QPushButton( "Create virtual port..." );
    box->addWidget( midiVirtualBtn_ );

    // ACTIVE since proposal 21 L2: a selected port is OPENED, and a track
    // whose `trackInput` names it hears what arrives on it. The list is the
    // hub's, not the out-pump's probe, so the computer keyboard appears in it
    // beside the hardware (design D9).
    box->addWidget( new QLabel(
        "Input ports (selected ports are opened for live play and recording):" ) );
    midiInputs_ = new QListWidget;
    midiInputs_->setSelectionMode( QAbstractItemView::MultiSelection );
    box->addWidget( midiInputs_, 1 );

    QFormLayout *form = new QFormLayout;
    midiOffsetMs_ = new QSpinBox;
    // The same +-500 ms the per-track offset allows, and the same sign: a
    // POSITIVE value sends EARLIER, which is what compensates outboard gear
    // whose audio return arrives late.
    midiOffsetMs_->setRange( -500, 500 );
    midiOffsetMs_->setSuffix( " ms" );
    form->addRow( "MIDI-out offset (+ = earlier):", midiOffsetMs_ );

    midiChaseNoteOns_ = new QCheckBox(
        "Re-attack sounding notes when starting or locating" );
    form->addRow( QString(), midiChaseNoteOns_ );
    form->addRow( new QLabel(
        "Controllers, program changes and pitch bend are always chased." ) );
    box->addLayout( form );

    return page;
}

void SOptionsDialog::loadMidiPage()
{
    SMidiOutPump *pump = SApplication::app().midiOutPump();

    midiOutputs_->clear();
    midiInputs_->clear();
    if( !pump ) {
        midiStatusLabel_->setText( "MIDI is not available." );
        midiVirtualBtn_->setEnabled( false );
        return;
    }

    const std::vector<audio::MidiPortInfo> outs = pump->outputPorts();
    for( const audio::MidiPortInfo &p : outs ) {
        QString label = QString::fromStdString( p.name );
        if( p.isVirtual ) label += "  (virtual)";
        midiOutputs_->addItem( label );
    }
    if( outs.empty() )
        midiOutputs_->addItem( "(no MIDI output ports on this machine)" );

    const QStringList wanted = SSettings::instance().midiInputPortIds();
    SMidiInputHub *hub = SApplication::app().midiInputHub();
    const std::vector<audio::MidiPortInfo> ins =
        hub ? hub->listPorts() : pump->inputPorts();
    for( const audio::MidiPortInfo &p : ins ) {
        QListWidgetItem *item =
            new QListWidgetItem( QString::fromStdString( p.name ), midiInputs_ );
        item->setData( Qt::UserRole, QString::fromStdString( p.id ) );
        if( wanted.contains( QString::fromStdString( p.id ) ) )
            item->setSelected( true );
        if( hub && hub->isOpen( QString::fromStdString( p.id ) ) )
            item->setToolTip( "Open." );
    }
    if( ins.empty() )
        midiInputs_->addItem( "(no MIDI input ports on this machine)" );

    // Gate the offer on the CAPABILITY, not on the platform: WinMM has no
    // virtual-port concept at all (a loopMIDI-style driver shows up as an
    // ordinary device instead), and a button that always failed would be worse
    // than no button.
    midiVirtualBtn_->setEnabled( pump->supportsVirtualPorts() );
    if( !pump->supportsVirtualPorts() )
        midiVirtualBtn_->setToolTip(
            "This MIDI system has no virtual ports. Install a loopback driver "
            "(loopMIDI) - its ports then appear in the list above." );

    SSettings &s = SSettings::instance();
    midiOffsetMs_->setValue( s.value( SOpt::MidiOutOffsetMs,
                                      SOpt::def( SOpt::MidiOutOffsetMs ) ).toInt() );
    midiChaseNoteOns_->setChecked(
        s.value( SOpt::MidiChaseNoteOns,
                 SOpt::def( SOpt::MidiChaseNoteOns ) ).toBool() );

    midiStatusLabel_->setText(
        QString( "MIDI system: %1 - %2 output port(s), %3 input port(s)." )
            .arg( pump->backendName() )
            .arg( (int) outs.size() )
            .arg( (int) ins.size() ) );
}

void SOptionsDialog::applyMidiPage()
{
    SSettings &s = SSettings::instance();
    s.setValue( SOpt::MidiOutOffsetMs, midiOffsetMs_->value() );
    s.setValue( SOpt::MidiChaseNoteOns, midiChaseNoteOns_->isChecked() );

    QStringList selected;
    for( int i = 0; i < midiInputs_->count(); ++i ) {
        QListWidgetItem *item = midiInputs_->item( i );
        const QString id = item->data( Qt::UserRole ).toString();
        if( item->isSelected() && !id.isEmpty() ) selected << id;
    }
    s.setMidiInputPortIds( selected );

    // OPEN what was selected (proposal 21 L2). Opening is idempotent and a
    // port is kept for the process, so this is "make sure it is listening"
    // rather than a toggle - and it is what makes the page ACTIVE instead of
    // an informational list. Deselecting does NOT close: a track may be armed
    // on that port right now, and the Options dialog is not where a monitoring
    // session gets torn down.
    if( SMidiInputHub *hub = SApplication::app().midiInputHub() )
        for( const QString &id : selected ) hub->fanoutFor( id );
}

QString SOptionsDialog::describeMidiPage() const
{
    QStringList lines;
    lines << QString( "backend=%1 virtual=%2" )
                 .arg( SApplication::app().midiOutPump()
                           ? SApplication::app().midiOutPump()->backendName()
                           : QStringLiteral( "none" ) )
                 .arg( midiVirtualBtn_->isEnabled() ? "yes" : "no" );
    lines << QString( "outputs=%1" ).arg( midiOutputs_->count() );
    for( int i = 0; i < midiOutputs_->count(); ++i )
        lines << QString( "  out[%1]=%2" ).arg( i ).arg( midiOutputs_->item( i )->text() );
    lines << QString( "inputs=%1" ).arg( midiInputs_->count() );
    for( int i = 0; i < midiInputs_->count(); ++i )
        lines << QString( "  in[%1]=%2 id=%3 selected=%4" )
                     .arg( i )
                     .arg( midiInputs_->item( i )->text() )
                     .arg( midiInputs_->item( i )->data( Qt::UserRole ).toString() )
                     .arg( midiInputs_->item( i )->isSelected() ? 1 : 0 );
    lines << QString( "offsetMs=%1" ).arg( midiOffsetMs_->value() );
    lines << QString( "chaseNoteOns=%1" ).arg( midiChaseNoteOns_->isChecked() ? 1 : 0 );
    return lines.join( "\n" );
}

void SOptionsDialog::loadMousePage()
{
    SSettings &s = SSettings::instance();
    selectByData( wheelPlain_,     s.value( SOpt::WheelPlain,     SOpt::def( SOpt::WheelPlain ) ) );
    selectByData( wheelShift_,     s.value( SOpt::WheelShift,     SOpt::def( SOpt::WheelShift ) ) );
    selectByData( wheelCtrl_,      s.value( SOpt::WheelCtrl,      SOpt::def( SOpt::WheelCtrl ) ) );
    selectByData( wheelCtrlShift_, s.value( SOpt::WheelCtrlShift, SOpt::def( SOpt::WheelCtrlShift ) ) );
    wheelSensitivity_->setValue( s.value( SOpt::WheelSensitivityPct,
                                          SOpt::def( SOpt::WheelSensitivityPct ) ).toInt() );
    zoomToCursor_->setChecked( s.value( SOpt::ZoomToCursor, SOpt::def( SOpt::ZoomToCursor ) ).toBool() );
    invertZoom_->setChecked(  s.value( SOpt::InvertZoom,   SOpt::def( SOpt::InvertZoom ) ).toBool() );
    followPlayhead_->setChecked( s.value( SOpt::FollowPlayhead, SOpt::def( SOpt::FollowPlayhead ) ).toBool() );
}

void SOptionsDialog::applyMousePage()
{
    SSettings &s = SSettings::instance();
    s.setValue( SOpt::WheelPlain,     wheelPlain_->currentData() );
    s.setValue( SOpt::WheelShift,     wheelShift_->currentData() );
    s.setValue( SOpt::WheelCtrl,      wheelCtrl_->currentData() );
    s.setValue( SOpt::WheelCtrlShift, wheelCtrlShift_->currentData() );
    s.setValue( SOpt::WheelSensitivityPct, wheelSensitivity_->value() );
    s.setValue( SOpt::ZoomToCursor,   zoomToCursor_->isChecked() );
    s.setValue( SOpt::InvertZoom,     invertZoom_->isChecked() );
    s.setValue( SOpt::FollowPlayhead, followPlayhead_->isChecked() );
}

// -------------------------------------------------------- Event Editor page

void SOptionsDialog::loadEventEditorPage()
{
    SSettings &s = SSettings::instance();
    selectByData( eventWheelPlain_,
        s.value( SOpt::EventWheelPlain, SOpt::def( SOpt::EventWheelPlain ) ) );
    selectByData( eventWheelShift_,
        s.value( SOpt::EventWheelShift, SOpt::def( SOpt::EventWheelShift ) ) );
    selectByData( eventWheelCtrl_,
        s.value( SOpt::EventWheelCtrl, SOpt::def( SOpt::EventWheelCtrl ) ) );
    selectByData( eventWheelCtrlShift_,
        s.value( SOpt::EventWheelCtrlShift, SOpt::def( SOpt::EventWheelCtrlShift ) ) );
    eventWheelSensitivity_->setValue( s.value( SOpt::EventWheelSensitivityPct,
        SOpt::def( SOpt::EventWheelSensitivityPct ) ).toInt() );
    eventZoomToCursor_->setChecked(
        s.value( SOpt::EventZoomToCursor, SOpt::def( SOpt::EventZoomToCursor ) ).toBool() );
    eventInvertZoom_->setChecked(
        s.value( SOpt::EventInvertZoom, SOpt::def( SOpt::EventInvertZoom ) ).toBool() );
}

void SOptionsDialog::applyEventEditorPage()
{
    SSettings &s = SSettings::instance();
    s.setValue( SOpt::EventWheelPlain,     eventWheelPlain_->currentData() );
    s.setValue( SOpt::EventWheelShift,     eventWheelShift_->currentData() );
    s.setValue( SOpt::EventWheelCtrl,      eventWheelCtrl_->currentData() );
    s.setValue( SOpt::EventWheelCtrlShift, eventWheelCtrlShift_->currentData() );
    s.setValue( SOpt::EventWheelSensitivityPct, eventWheelSensitivity_->value() );
    s.setValue( SOpt::EventZoomToCursor,   eventZoomToCursor_->isChecked() );
    s.setValue( SOpt::EventInvertZoom,     eventInvertZoom_->isChecked() );
}

// "Lautsprecher (US-16x08) — 48000 Hz". The rate is a per-ENDPOINT Windows
// setting, and an input endpoint at one rate with an output endpoint at another
// on the SAME interface makes the OS resample one side and misreport its clock
// — which no arithmetic downstream can undo. Putting the number in the label is
// the cheapest way to make a mismatch visible AT THE POINT OF CHOICE, next to
// the other one. Rate 0 = the backend could not read it; say nothing rather
// than guess.
static QString sDeviceLabel( const std::string &name, std::uint32_t rate )
{
    const QString n = QString::fromStdString( name );
    if( rate == 0 ) return n;
    return QStringLiteral( "%1 — %2 Hz" ).arg( n ).arg( rate );
}

void SOptionsDialog::loadAudioPage()
{
    // Load output devices
    audioDevice_->clear();
    auto spk = SApplication::app().getSpeaker();
    std::vector<audio::AudioDeviceInfo> devs = spk ? spk->outputDevices()
                                                   : std::vector<audio::AudioDeviceInfo>();
    if( devs.empty() ) {
        audioDevice_->addItem( "System default", "default" );
    } else {
        for( const audio::AudioDeviceInfo &d : devs ) {
            audioDevice_->addItem( sDeviceLabel( d.name, d.sampleRate ),
                                   QString::fromStdString( d.id ) );
        }
    }
    QString cur = spk ? QString::fromStdString( spk->outputDevice() )
                      : SSettings::instance().audioDeviceId();
    int i = audioDevice_->findData( cur );
    if( i >= 0 ) audioDevice_->setCurrentIndex( i );

    // Load input devices. REAL since proposal 21 L1b / main PR #54: the same
    // createAudioInput() the live lane uses, so the list is the backend the env
    // selected (a headless run sees the file or null backend, a desktop one
    // sees WASAPI / ALSA / CoreAudio). The label carries the endpoint's
    // shared-mode MIX RATE and channel count (main PR #55): a rate mismatch
    // between the input and the output is then visible AT THE POINT OF CHOICE.
    // The probe device is opened and closed here and nowhere else; a failure
    // to enumerate leaves just "System default", which is what it did before.
    audioInputDevice_->clear();
    audioInputDevice_->addItem( "System default", "default" );
    {
        std::unique_ptr<audio::AudioInput> probe = audio::createAudioInput();
        if( probe ) {
            for( const audio::AudioInputDeviceInfo &d : probe->listDevices() ) {
                const QString id   = QString::fromStdString( d.id );
                if( id.isEmpty() || id == QStringLiteral( "default" ) ) continue;
                // channels==0 (an ASIO entry — no driver loaded to ask it) may
                // already be known from a PAST probe (onAudioInputDeviceSelected
                // below); use that cached count for the label so a reopened
                // dialog does not look like it forgot.
                uint32_t channels = d.channels;
                if( channels == 0 )
                    channels = SSettings::instance().audioInputChannelCount( id );
                QString label = sDeviceLabel( d.name, d.sampleRate );
                if( channels > 0 )
                    label = QStringLiteral( "%1 (%2 ch)" ).arg( label ).arg( channels );
                audioInputDevice_->addItem( label, id );
                audioInputDevice_->setItemData(
                    audioInputDevice_->count() - 1, channels, Qt::UserRole + 1 );
            }
        }
    }
    QString curIn = SSettings::instance().audioInputDeviceId();
    if( curIn.isEmpty() ) curIn = "default";
    int j = audioInputDevice_->findData( curIn );
    if( j >= 0 ) audioInputDevice_->setCurrentIndex( j );
    if( recordingOffsetMs_ )
        recordingOffsetMs_->setValue(
            (int) SSettings::instance().recordingOffsetMs( curIn ) );
    if( metronomeLevel_ )
        metronomeLevel_->setValue( (int) qRound(
            100.0 * SSettings::instance()
                        .value( SOpt::MetronomeLevel,
                                SOpt::def( SOpt::MetronomeLevel ) ).toDouble() ) );
    if( countInBars_ )
        countInBars_->setValue( SSettings::instance()
                                    .value( SOpt::CountInBars,
                                            SOpt::def( SOpt::CountInBars ) ).toInt() );
    if( preRollBars_ )
        preRollBars_->setValue( SSettings::instance()
                                    .value( SOpt::PreRollBars,
                                            SOpt::def( SOpt::PreRollBars ) ).toInt() );

    // Load latencies (cached from startup) and buffer size options
    if( spk ) {
        audio::AudioBackend *backend = spk->getBackend();

        // Display output latency (from cache)
        QString outDeviceId = audioDevice_->currentData().toString();
        uint32_t cachedOutLatency = SSettings::instance().audioOutputLatencyFrames( outDeviceId );
        if( cachedOutLatency > 0 ) {
            uint32_t sampleRate = backend ? backend->getConfig().sampleRate : 48000;
            double ms = static_cast<double>(cachedOutLatency) / sampleRate * 1000.0;
            outputLatencyLabel_->setText( QString::asprintf( "~%.1f ms (%u frames)", ms, cachedOutLatency ) );
        } else {
            outputLatencyLabel_->setText( "(not yet measured)" );
        }

        // Display input latency (from cache)
        QString inDeviceId = audioInputDevice_->currentData().toString();
        uint32_t cachedInLatency = SSettings::instance().audioInputLatencyFrames( inDeviceId );
        if( cachedInLatency > 0 ) {
            // Estimate sample rate for display (use 48kHz if unknown)
            uint32_t sampleRate = 48000;
            if( backend ) sampleRate = backend->getConfig().sampleRate;
            double ms = static_cast<double>(cachedInLatency) / sampleRate * 1000.0;
            inputLatencyLabel_->setText( QString::asprintf( "~%.1f ms (%u frames)", ms, cachedInLatency ) );
        } else {
            inputLatencyLabel_->setText( "(not yet measured)" );
        }

        // Load available buffer sizes
        if( backend ) {
            bufferSizeCombo_->clear();
            std::vector<uint32_t> sizes = backend->getAvailableBufferSizes();
            if( sizes.empty() ) {
                bufferSizeCombo_->addItem( QString::number( backend->getConfig().bufferFrames )
                                          + " frames (device-managed)" );
                bufferSizeCombo_->setEnabled( false );
            } else {
                for( uint32_t size : sizes ) {
                    bufferSizeCombo_->addItem( QString::number( size ) + " frames", size );
                }
                uint32_t current = backend->getConfig().bufferFrames;
                int idx = -1;
                for( int k = 0; k < bufferSizeCombo_->count(); ++k ) {
                    if( bufferSizeCombo_->itemData( k ).toUInt() == current ) {
                        idx = k;
                        break;
                    }
                }
                if( idx >= 0 ) bufferSizeCombo_->setCurrentIndex( idx );
                bufferSizeCombo_->setEnabled( true );
            }
        }

    } else {
        outputLatencyLabel_->setText( "(not available)" );
        inputLatencyLabel_->setText( "(not available)" );
        bufferSizeCombo_->setEnabled( false );
    }

    // The panel button is shown only for a backend that HAS a panel. There is
    // no "does it?" query that does not open one, so the id prefix is the
    // test: `asio:` has one, everything else does not. A driver that turns out
    // to have none answers ASE_NotPresent and the click is a no-op with a log
    // line, which is the honest outcome for a question only the driver can
    // answer.
    if( driverPanelBtn_ ) {
        const QString outId = audioDevice_->currentData().toString();
        const bool isAsio = outId.startsWith( QStringLiteral( "asio:" ),
                                              Qt::CaseInsensitive );
        driverPanelBtn_->setVisible( isAsio );
        driverPanelBtn_->setEnabled( isAsio );
    }
}

void SOptionsDialog::onMeasureLoopback()
{
    const QString outId = audioDevice_->currentData().toString();
    const QString inId  = audioInputDevice_->currentData().toString();

    // ONE DEVICE FOR BOTH DIRECTIONS, and this is not fussiness. The
    // measurement counts frames from each stream's OWN start, so if the two
    // streams started at different moments the difference carries that skew
    // and is not a latency at all. On a full-duplex ASIO driver one callback
    // serves both directions, so the two counters share an origin by
    // construction; two separate endpoints do not. The runner refuses it as
    // well — this check exists only so the refusal arrives as a sentence
    // rather than as a failed run.
    if( outId.isEmpty() || outId != inId ) {
        QMessageBox::information(
            this, "Measure round trip",
            "This measurement needs ONE device selected for BOTH the output and "
            "the input.\n\n"
            "It counts frames from each stream's own start, so two separately "
            "started streams would contribute the gap between their start times "
            "as if it were latency. A full-duplex ASIO driver serves both "
            "directions from one callback, which is what makes the two counts "
            "comparable." );
        return;
    }

    // Not while the transport owns the devices: the run starts and stops them.
    if( SApplication::app().isPlaying() || SApplication::app().isRecordingActive() ) {
        QMessageBox::information( this, "Measure round trip",
                                  "Stop playback and recording first — the measurement "
                                  "needs to start and stop the device itself." );
        return;
    }

    if( QMessageBox::question(
            this, "Measure round trip",
            "Connect a cable from an OUTPUT of this device to the INPUT you record "
            "from, then continue.\n\n"
            "A short click will be played. The app will be unresponsive for about "
            "three seconds.\n\n"
            "Nothing is changed without your say-so: the result is shown first.",
            QMessageBox::Ok | QMessageBox::Cancel ) != QMessageBox::Ok )
        return;

    // FRESH device objects rather than the app's. On ASIO the registry hands
    // back the same underlying driver anyway (proposal 35), so this measures
    // the device the user actually selected without disturbing whatever the
    // speaker is holding.
    std::unique_ptr<audio::AudioBackend> be = audio::createAudioBackend();
    std::unique_ptr<audio::AudioInput>   in = audio::createAudioInput();
    if( !be || !in ) return;

    audio::LoopbackRunParams p;
    p.outputDeviceId   = outId.toStdString();
    p.inputDeviceId    = inId.toStdString();
    p.inputChannel     = 0;
    p.inputChannelMask = 1;

    in->requestChannels( p.inputChannelMask );
    if( in->openDevice( p.inputDeviceId, 0 ) != 0 ) {
        QMessageBox::warning( this, "Measure round trip",
                              QString( "The input device would not open:\n%1" )
                                  .arg( QString::fromUtf8( in->errorMessage() ) ) );
        return;
    }
    if( be->openDevice( p.outputDeviceId, 0 ) != 0 ) {
        QMessageBox::warning( this, "Measure round trip",
                              "The output device would not open." );
        return;
    }

    // The run BLOCKS for the capture window plus the driver's start-up (~0.5 s
    // on the hardware this was built against). A wait cursor is the honest
    // signal; a progress dialog would not repaint anyway, because nothing
    // returns to the event loop until the pass is done.
    QApplication::setOverrideCursor( Qt::WaitCursor );
    const audio::LoopbackRun r = audio::runLoopback( be.get(), in.get(), p );
    QApplication::restoreOverrideCursor();

    be->closeDevice();
    in->closeDevice();

    if( !r.result.found ) {
        QString why = QString::fromStdString( r.error );
        if( why.isEmpty() ) why = "No probe was found in the capture.";
        QMessageBox::warning(
            this, "Measure round trip",
            why + "\n\n" +
                QString::fromUtf8( audio::loopbackLevelAdvice(
                    audio::loopbackLevelOf( r.result ) ) ) );
        return;
    }

    const double offerMs = r.suggestedOffsetMs();
    const audio::LoopbackLevel level = audio::loopbackLevelOf( r.result );

    QString body =
        QString( "Round trip: %1 frames (%2 ms)\n"
                 "The driver reports %3 out + %4 in = %5 frames.\n\n"
                 "Residual it did NOT report: %6 ms." )
            .arg( (qlonglong) r.result.roundTripFrames )
            .arg( audio::loopbackMs( r.result.roundTripFrames, r.sampleRate ), 0, 'f', 2 )
            .arg( r.reportedOutputFrames )
            .arg( r.reportedInputFrames )
            .arg( r.reportedOutputFrames + r.reportedInputFrames )
            .arg( offerMs, 0, 'f', 2 );

    // A WEAK return is reported even though the measurement SUCCEEDED. The
    // first real measurement on hardware passed at 1.5x the refusal floor —
    // correct, but one gain step away from being refused — and a user deciding
    // whether to trust a number deserves to know that.
    if( level != audio::LoopbackLevel::Good )
        body += QString( "\n\nNote: %1." )
                    .arg( QString::fromUtf8( audio::loopbackLevelAdvice( level ) ) );

    body += QString( "\n\nSet the recording offset to %1 ms?" )
                .arg( offerMs, 0, 'f', 0 );

    // IT PROPOSES; THE USER DISPOSES. And even "Yes" only fills the spin box —
    // the value is not written until the dialog itself is applied, so there
    // are two deliberate steps between a measurement and a timing constant
    // that shifts every take recorded afterwards.
    if( QMessageBox::question( this, "Measure round trip", body,
                               QMessageBox::Yes | QMessageBox::No ) == QMessageBox::Yes ) {
        if( recordingOffsetMs_ )
            recordingOffsetMs_->setValue( (int) qRound( offerMs ) );
    }
}

void SOptionsDialog::onAudioInputDeviceSelected( int index )
{
    if( index < 0 || !audioInputDevice_ ) return;
    // Already known — either a WASAPI endpoint (listDevices() reads its mix
    // format for free) or an ASIO device this dialog probed before. Nothing to
    // open.
    if( audioInputDevice_->itemData( index, Qt::UserRole + 1 ).toUInt() > 0 ) return;

    const QString id = audioInputDevice_->currentData().toString();
    if( id.isEmpty() || id == QStringLiteral( "default" ) ) return;

    // Opening an ASIO driver just to learn its channel count can briefly
    // block (driver init, occasionally a splash screen) — acceptable HERE,
    // a deliberate "configure this device" gesture in a modal settings
    // dialog, and never from the arm button's context menu, which can appear
    // mid-session during playback or recording.
    QApplication::setOverrideCursor( Qt::WaitCursor );
    std::unique_ptr<audio::AudioInput> probe = audio::createAudioInput();
    uint32_t channels = 0;
    // Rate 0 == "no preference": this is counting channels, and a non-zero
    // preferredRate makes an ASIO driver SET its rate. deviceInputChannels()
    // rather than getConfig().channels, which is the width as OPENED — one
    // channel on ASIO until something demands more.
    if( probe && probe->openDevice( id.toStdString(), 0 ) == 0 )
        channels = probe->deviceInputChannels();
    QApplication::restoreOverrideCursor();
    if( channels == 0 ) return;   // driver refused to open; leave it unmarked

    SSettings::instance().setAudioInputChannelCount( id, channels );
    audioInputDevice_->setItemData( index, channels, Qt::UserRole + 1 );
    audioInputDevice_->setItemText( index,
        QStringLiteral( "%1 (%2 ch)" ).arg( audioInputDevice_->itemText( index ) )
                                       .arg( channels ) );
}

void SOptionsDialog::onOpenDriverPanel()
{
    auto spk = SApplication::app().getSpeaker();
    audio::AudioBackend *backend = spk ? spk->getBackend() : nullptr;
    if( !backend ) return;

    // The device must be OPEN for the driver to have a panel to show: the
    // panel belongs to an instantiated driver, not to a CLSID. When nothing is
    // playing there is no open device, so say that rather than appearing to do
    // nothing.
    if( backend->getConfig().sampleRate == 0 ) {
        QMessageBox::information(
            this, "Driver Control Panel",
            "The audio device is not open yet.\n\n"
            "Press Play once (or arm a track) so the driver is running, then "
            "open this panel." );
        return;
    }

    // BLOCKS for as long as the user leaves the driver's window open. That is
    // the driver's call being modal, not a choice here; every host behaves
    // this way.
    const int rc = backend->openControlPanel();
    if( rc != 0 ) {
        QMessageBox::information(
            this, "Driver Control Panel",
            "This driver does not offer a control panel." );
        return;
    }

    // Re-read: the whole point of the panel on a fixed-buffer driver is that
    // the number it shows is the one that just changed.
    loadAudioPage();
}

void SOptionsDialog::applyAudioPage()
{
    // Save output device
    QString id = audioDevice_->currentData().toString();
    if( !id.isEmpty() ) {
        if( auto spk = SApplication::app().getSpeaker() ) {
            // A genuinely different choice gets its own first chance at the
            // "audio device unavailable" dialog — see
            // SApplication::notifyAudioOutputUnavailable(). Applying the page
            // with the SAME device re-selected must not reset it, or a user
            // stuck on a still-broken device could re-trigger the dialog by
            // opening and closing Options with no actual change.
            if( QString::fromStdString( spk->outputDevice() ) != id )
                SApplication::app().clearAudioOutputFailureNotice();
            spk->setOutputDevice( id.toStdString() );
        }
        SSettings::instance().setAudioDeviceId( id );
    }

    // Save input device
    QString inId = audioInputDevice_->currentData().toString();
    if( !inId.isEmpty() && recordingOffsetMs_ ) {
        // Written against the device the combo NOW names, so changing both in
        // one visit stores the offset for the device it was typed for.
        SSettings::instance().setRecordingOffsetMs(
            inId, (double) recordingOffsetMs_->value() );
    }
    if( !inId.isEmpty() ) {
        const bool moved = SSettings::instance().audioInputDeviceId() != inId;
        SSettings::instance().setAudioInputDeviceId( inId );
        // A DEVICE CHANGE is a live-plan rebuild trigger (proposal 21 design
        // section 3): a track whose trackInput names no device follows this
        // setting, and the monitor has to re-open the input for it. Nothing
        // else in the app is watching this key.
        if( moved ) SApplication::app().liveLanesChanged();
    }

    // Transport polish (proposal 21 L5). The level is a plan-rebuild trigger,
    // and it arrives through the plan SIGNATURE the demand tick compares
    // (SLiveMonitor::planSignature) rather than through a signal - the same
    // arrangement a fader move uses.
    if( metronomeLevel_ )
        SSettings::instance().setValue( SOpt::MetronomeLevel,
                                        metronomeLevel_->value() / 100.0 );
    if( countInBars_ )
        SSettings::instance().setValue( SOpt::CountInBars, countInBars_->value() );
    if( preRollBars_ )
        SSettings::instance().setValue( SOpt::PreRollBars, preRollBars_->value() );

    // Apply buffer size change (if supported)
    if( auto spk = SApplication::app().getSpeaker() ) {
        audio::AudioBackend *backend = spk->getBackend();
        if( backend && bufferSizeCombo_->isEnabled() &&
            bufferSizeCombo_->currentData().toUInt() > 0 ) {
            uint32_t newSize = bufferSizeCombo_->currentData().toUInt();
            uint32_t currentSize = backend->getConfig().bufferFrames;
            if( newSize != currentSize ) {
                // Buffer size can only be changed when not playing
                if( !spk->isPlaying() ) {
                    int rc = backend->setBufferSize( newSize );
                    if( rc != 0 ) {
                        qWarning( "Failed to change buffer size to %u frames", newSize );
                    }
                } else {
                    qWarning( "Cannot change buffer size while playing. Stop playback and try again." );
                }
            }
        }
    }
}

void SOptionsDialog::apply()
{
    applyMousePage();
    applyEventEditorPage();
    applyAudioPage();
    applyMidiPage();
    applyLogPage();
    applyPluginsPage();
}

// ---------------------------------------------------------------- Log page

QWidget *SOptionsDialog::buildLogPage()
{
    QWidget *page = new QWidget;
    QFormLayout *form = new QFormLayout( page );

    logConsole_ = new QCheckBox( "Also write the log to the console (stderr)" );
    logConsole_->setToolTip(
        "Defaults to on for a debug build and off for a release build. "
        "--log-console / --no-log-console on the command line, and the "
        "SMARAGD_LOG_CONSOLE environment variable, override this." );
    form->addRow( logConsole_ );

    logLevel_ = new QComboBox;
    logLevel_->addItem( "Errors only", "error" );
    logLevel_->addItem( "Warnings",    "warn" );
    logLevel_->addItem( "Info",        "info" );
    logLevel_->addItem( "Debug",       "debug" );
    logLevel_->addItem( "Trace",       "trace" );
    logLevel_->setToolTip( "Records below this level are discarded at the call "
                           "site, so a lower setting also costs less." );
    form->addRow( "Level:", logLevel_ );

    logCapacity_ = new QSpinBox;
    logCapacity_->setRange( 1000, 5000000 );
    logCapacity_->setSingleStep( 10000 );
    logCapacity_->setGroupSeparatorShown( true );
    logCapacity_->setToolTip(
        "How many records the in-memory buffer keeps for the log window. "
        "Memory is only used as records are actually written. Changing this "
        "clears the buffer." );
    form->addRow( "Buffer (records):", logCapacity_ );

    logToFile_ = new QCheckBox( "Write a rotating log file" );
    logToFile_->setToolTip( "smaragd.log next to smaragd.ini, rotated at 8 MB, "
                            "3 backups kept. Takes effect on restart." );
    form->addRow( logToFile_ );

    logPathLabel_ = new QLabel;
    logPathLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    form->addRow( "Folder:", logPathLabel_ );

    loadLogPage();
    return page;
}

void SOptionsDialog::loadLogPage()
{
    SSettings &s = SSettings::instance();
    logConsole_->setChecked(
        s.value( SOpt::LogConsole, SOpt::def( SOpt::LogConsole ) ).toBool() );
    selectByData( logLevel_,
        s.value( SOpt::LogLevel, SOpt::def( SOpt::LogLevel ) ).toString() );
    logCapacity_->setValue(
        s.value( SOpt::LogCapacity, SOpt::def( SOpt::LogCapacity ) ).toInt() );
    logToFile_->setChecked(
        s.value( SOpt::LogToFile, SOpt::def( SOpt::LogToFile ) ).toBool() );
    logPathLabel_->setText( s.configDir() );
}

void SOptionsDialog::applyLogPage()
{
    SSettings &s = SSettings::instance();
    const bool    console = logConsole_->isChecked();
    const QString level   = logLevel_->currentData().toString();
    const int     cap     = logCapacity_->value();

    s.setValue( SOpt::LogConsole,  console );
    s.setValue( SOpt::LogLevel,    level );
    s.setValue( SOpt::LogCapacity, cap );
    s.setValue( SOpt::LogToFile,   logToFile_->isChecked() );

    // Console and level are live — a user turning on Trace to chase something
    // should not have to restart to see it. The file sink is not, because
    // starting or stopping its writer mid-session is not worth the complexity
    // for a setting nobody changes twice.
    tw::TwLog &log = tw::TwLog::instance();
    log.setConsole( console );
    if     ( level == "error" ) log.setMinLevel( tw::LogLevel::Error );
    else if( level == "warn"  ) log.setMinLevel( tw::LogLevel::Warn );
    else if( level == "info"  ) log.setMinLevel( tw::LogLevel::Info );
    else if( level == "trace" ) log.setMinLevel( tw::LogLevel::Trace );
    else                        log.setMinLevel( tw::LogLevel::Debug );

    // Resizing the ring discards it, so only do it on an actual change.
    if( static_cast<int>( log.capacity() ) != cap ) log.setCapacity( cap );
}

// ------------------------------------------------------------ Plugins page
// Proposal 08 M2, AC 1: the search-path list is editable and persisted, the
// scan is cached between startups, and a rescan can be triggered from here.

QWidget *SOptionsDialog::buildPluginsPage()
{
    QWidget     *page = new QWidget;
    QVBoxLayout *v    = new QVBoxLayout( page );

    v->addWidget( new QLabel( "Directories searched for plugins (CLAP), "
                              "including sub-directories:" ) );

    pluginDirs_ = new QListWidget;
    pluginDirs_->setSelectionMode( QAbstractItemView::ExtendedSelection );
    pluginDirs_->setAlternatingRowColors( true );
    pluginDirs_->setToolTip( "A directory that does not exist is skipped, not an "
                             "error — it stays in the list so a plugin installed "
                             "later is found without re-adding it." );
    v->addWidget( pluginDirs_, 1 );

    QPushButton *addBtn = new QPushButton( "Add…" );
    pluginRemoveBtn_    = new QPushButton( "Remove" );
    QPushButton *defBtn = new QPushButton( "Defaults" );
    defBtn->setToolTip( "Restore this platform's standard plugin locations." );

    QHBoxLayout *btns = new QHBoxLayout;
    btns->addWidget( addBtn );
    btns->addWidget( pluginRemoveBtn_ );
    btns->addWidget( defBtn );
    btns->addStretch();
    v->addLayout( btns );

    pluginScanOnStartup_ = new QCheckBox( "Scan for plugins at startup" );
    pluginScanOnStartup_->setToolTip(
        "The result is cached in plugincache.v<n>.json next to smaragd.ini, keyed "
        "on each module's path, size and modification time — so only plugins that "
        "actually changed are loaded again. The <n> is the scanner version, so "
        "two Smaragd builds cannot invalidate each other's cache." );
    v->addWidget( pluginScanOnStartup_ );

    pluginRescanBtn_ = new QPushButton( "Rescan now" );
    pluginRescanBtn_->setToolTip(
        "Applies the list above and re-probes EVERY module, including ones whose "
        "previous probe crashed or timed out (those are otherwise remembered and "
        "skipped, so one bad plugin does not slow down every launch)." );
    pluginStatusLabel_ = new QLabel;
    pluginStatusLabel_->setWordWrap( true );

    QHBoxLayout *row = new QHBoxLayout;
    row->addWidget( pluginRescanBtn_ );
    row->addWidget( pluginStatusLabel_, 1 );
    v->addLayout( row );

    QObject::connect( addBtn, &QPushButton::clicked,
                      this, &SOptionsDialog::addPluginDir );
    QObject::connect( pluginRemoveBtn_, &QPushButton::clicked,
                      this, &SOptionsDialog::removePluginDirs );
    QObject::connect( defBtn, &QPushButton::clicked,
                      this, &SOptionsDialog::resetPluginDirsToDefaults );
    QObject::connect( pluginRescanBtn_, &QPushButton::clicked,
                      this, &SOptionsDialog::rescanPluginsNow );
    QObject::connect( &SApplication::app(), &SApplication::pluginScanFinished,
                      this, &SOptionsDialog::updatePluginScanStatus );

    // The engine's scanner runs on a worker thread and deliberately does NOT
    // emit Qt signals, so the live progress line is a main-thread poll. It is
    // also what shows a scan already running when this dialog opens.
    pluginStatusTimer_ = new QTimer( this );
    pluginStatusTimer_->setInterval( 400 );
    QObject::connect( pluginStatusTimer_, &QTimer::timeout,
                      this, &SOptionsDialog::updatePluginScanStatus );
    pluginStatusTimer_->start();

    loadPluginsPage();
    return page;
}

void SOptionsDialog::loadPluginsPage()
{
    SSettings &s = SSettings::instance();
    pluginDirs_->clear();
    pluginDirs_->addItems( s.pluginSearchPaths() );
    pluginScanOnStartup_->setChecked( s.pluginScanOnStartup() );
    updatePluginScanStatus();
}

void SOptionsDialog::applyPluginsPage()
{
    QStringList dirs;
    for( int i = 0; i < pluginDirs_->count(); ++i )
        dirs << pluginDirs_->item( i )->text();

    SSettings &s = SSettings::instance();
    s.setPluginSearchPaths( dirs );
    s.setPluginScanOnStartup( pluginScanOnStartup_->isChecked() );

    // Live: the registry follows the list immediately, so the next scan (from
    // "Rescan now" or the next launch) uses it without a restart.
    SApplication::app().pushPluginSearchPaths();
}

void SOptionsDialog::addPluginDir()
{
    SSettings &s = SSettings::instance();
    const QString start = s.lastDir( "plugins" );
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Add a plugin directory", start );
    if( dir.isEmpty() ) return;

    s.setLastDir( "plugins", dir );

    const QString clean = QDir::cleanPath( dir );
    // Case-insensitively de-duplicated: on Windows the same folder reached two
    // ways would otherwise be scanned twice.
    if( !pluginDirs_->findItems( clean, Qt::MatchFixedString ).isEmpty() ) return;
    pluginDirs_->addItem( clean );
}

void SOptionsDialog::removePluginDirs()
{
    const QList<QListWidgetItem *> sel = pluginDirs_->selectedItems();
    for( QListWidgetItem *it : sel ) delete it;
}

void SOptionsDialog::resetPluginDirsToDefaults()
{
    // Must stay in step with SOpt::def( PluginSearchPaths ) — same formats, same
    // de-duplication, or "restore defaults" would silently mean something else
    // than the shipped default.
    pluginDirs_->clear();
    QStringList dirs;
    for( const char *fmt : { "clap", "vst3" } )
        for( const std::string &d : audio::twPluginSearchPaths::defaults( fmt ) )
            dirs << QString::fromStdString( d );
    dirs.removeDuplicates();
    for( const QString &d : dirs ) pluginDirs_->addItem( d );
}

void SOptionsDialog::rescanPluginsNow()
{
    applyPluginsPage();
    // force = true: this is the user explicitly saying "try again", which is the
    // only thing that clears a remembered failed/timeout record.
    SApplication::app().rescanPlugins( true );
    pluginStatusLabel_->setText( "Scanning…" );
    pluginStatusTimer_->start();
}

void SOptionsDialog::updatePluginScanStatus()
{
    SApplication &app = SApplication::app();
    pluginStatusLabel_->setText( app.pluginScanStatusText() );
    pluginRescanBtn_->setEnabled( !app.isPluginScanActive() );
}

// ---------------------------------------------------------------- Media page
// (proposal 38 GATE 5b, design §C GATE 5.) Mirrors the Plugins page's LIVE
// pattern: Add/Save/Remove/Test connection commit straight through
// SApplication::mediaAccounts() rather than through OK/Apply, because the
// state that matters — the account list, the registered SWebDavMediaSource —
// lives on that long-lived manager. This is also what keeps a testkit verb
// that calls the manager directly and a real button click on the SAME code
// path: `onMediaSaveAccount()` is exactly what `SMediaAccountManager::
// setAccount()` returning false looks like from either caller (AC 13).

QWidget *SOptionsDialog::buildMediaPage()
{
    QWidget     *page = new QWidget;
    QVBoxLayout *v    = new QVBoxLayout( page );

    mediaBackendLabel_ = new QLabel;
    mediaBackendLabel_->setWordWrap( true );
    v->addWidget( mediaBackendLabel_ );

    v->addWidget( new QLabel( "Nextcloud accounts:" ) );
    mediaAccountList_ = new QListWidget;
    mediaAccountList_->setSelectionMode( QAbstractItemView::SingleSelection );
    v->addWidget( mediaAccountList_, 1 );

    QFormLayout *form = new QFormLayout;
    mediaAccountId_ = new QLineEdit;
    mediaAccountId_->setToolTip(
        "A short name for this connection (\"home\", \"band-share\", ...). "
        "Becomes the browser's source id, nextcloud:<this>." );
    form->addRow( "Account id:", mediaAccountId_ );
    mediaUrl_ = new QLineEdit;
    mediaUrl_->setPlaceholderText(
        "https://cloud.example.com/remote.php/dav/files/<username>/" );
    form->addRow( "Server URL:", mediaUrl_ );
    mediaUser_ = new QLineEdit;
    form->addRow( "Username:", mediaUser_ );
    mediaPassword_ = new QLineEdit;
    mediaPassword_->setEchoMode( QLineEdit::Password );
    mediaPassword_->setToolTip(
        "A Nextcloud app password (Settings -> Security -> Devices & sessions -> "
        "Create new app password on the server), not your login password." );
    form->addRow( "App password:", mediaPassword_ );
    v->addLayout( form );

    mediaRemember_ = new QCheckBox( "Remember this password" );
    v->addWidget( mediaRemember_ );

    QHBoxLayout *btns = new QHBoxLayout;
    mediaAddBtn_    = new QPushButton( "Add" );
    mediaSaveBtn_   = new QPushButton( "Save" );
    mediaRemoveBtn_ = new QPushButton( "Remove" );
    mediaTestBtn_   = new QPushButton( "Test connection" );
    btns->addWidget( mediaAddBtn_ );
    btns->addWidget( mediaSaveBtn_ );
    btns->addWidget( mediaRemoveBtn_ );
    btns->addStretch();
    btns->addWidget( mediaTestBtn_ );
    v->addLayout( btns );

    mediaTestResultLabel_ = new QLabel;
    mediaTestResultLabel_->setWordWrap( true );
    v->addWidget( mediaTestResultLabel_ );

    QObject::connect( mediaAccountList_, &QListWidget::currentItemChanged,
                      this, &SOptionsDialog::onMediaAccountSelected );
    QObject::connect( mediaAddBtn_, &QPushButton::clicked,
                      this, &SOptionsDialog::onMediaAddAccount );
    QObject::connect( mediaSaveBtn_, &QPushButton::clicked,
                      this, &SOptionsDialog::onMediaSaveAccount );
    QObject::connect( mediaRemoveBtn_, &QPushButton::clicked,
                      this, &SOptionsDialog::onMediaRemoveAccount );
    QObject::connect( mediaTestBtn_, &QPushButton::clicked,
                      this, &SOptionsDialog::onMediaTestConnection );

    loadMediaPage();
    return page;
}

void SOptionsDialog::refreshMediaAccountList()
{
    const QString wasSelected = mediaAccountId_->text();
    mediaAccountList_->blockSignals( true );
    mediaAccountList_->clear();
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    for( const QString &id : mgr->accountIds() )
        mediaAccountList_->addItem( id );
    mediaAccountList_->blockSignals( false );

    for( int i = 0; i < mediaAccountList_->count(); ++i ) {
        if( mediaAccountList_->item( i )->text() == wasSelected ) {
            mediaAccountList_->setCurrentRow( i );
            return;
        }
    }
}

void SOptionsDialog::clearMediaForm()
{
    mediaAccountId_->clear();
    mediaAccountId_->setEnabled( true );   // an id is only fixed once SAVED
    mediaUrl_->clear();
    mediaUser_->clear();
    mediaPassword_->clear();
    mediaPassword_->setPlaceholderText( QString() );
    mediaRemember_->setChecked( SApplication::app().mediaAccounts()->canRememberPasswords() );
    mediaTestResultLabel_->clear();
}

void SOptionsDialog::loadMediaAccountIntoForm( const QString &accountId )
{
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    const SMediaAccountInfo info = mgr->account( accountId );
    mediaAccountId_->setText( info.accountId );
    mediaAccountId_->setEnabled( false );   // renaming is remove + re-add
    mediaUrl_->setText( info.url );
    mediaUser_->setText( info.user );
    mediaPassword_->clear();   // never re-populated: the secret is never read back into the UI
    // ...which leaves an empty box that looks like a missing password. Say
    // which it is, in the one place the user is looking (Test connection
    // uses the saved one when this is left alone).
    switch( info.passwordStatus ) {
    case SMediaPasswordStatus::Set:
        mediaPassword_->setPlaceholderText( "(saved - type here only to replace it)" );
        break;
    case SMediaPasswordStatus::Undecryptable:
        mediaPassword_->setPlaceholderText( "(the saved password cannot be read here - re-enter it)" );
        break;
    case SMediaPasswordStatus::Unset:
        mediaPassword_->setPlaceholderText( QString() );
        break;
    }
    mediaRemember_->setChecked( info.passwordStatus != SMediaPasswordStatus::Unset );
    mediaRemember_->setEnabled( mgr->canRememberPasswords() );
    mediaTestResultLabel_->clear();
}

void SOptionsDialog::loadMediaPage()
{
    // Re-check for a legacy plaintext password key every time the page loads
    // (AC 17): the real trigger — an app LAUNCH finding one on disk — already
    // ran once at startup, so this is a cheap, harmless re-check that also
    // gives a qxa case a way to observe the migration without restarting the
    // process.
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    mgr->rescanForPlaintextMigration();

    mediaBackendLabel_->setText( mgr->backendDescription() );
    mediaRemember_->setEnabled( mgr->canRememberPasswords() );

    refreshMediaAccountList();
    if( mediaAccountList_->currentItem() == nullptr ) clearMediaForm();
}

void SOptionsDialog::onMediaAccountSelected( QListWidgetItem *current, QListWidgetItem * )
{
    if( !current ) { clearMediaForm(); return; }
    loadMediaAccountIntoForm( current->text() );
}

void SOptionsDialog::onMediaAddAccount()
{
    mediaAccountList_->setCurrentItem( nullptr );
    clearMediaForm();
}

void SOptionsDialog::onMediaSaveAccount()
{
    const QString accountId = mediaAccountId_->text().trimmed();
    if( accountId.isEmpty() ) {
        mediaTestResultLabel_->setText( "An account id is required." );
        return;
    }

    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    QString error;
    const bool ok = mgr->setAccount( accountId, mediaUrl_->text().trimmed(),
                                     mediaUser_->text(), mediaPassword_->text(),
                                     mediaRemember_->isChecked(), &error );
    if( !ok ) {
        mediaTestResultLabel_->setText( "Not saved: " + error );
        return;
    }

    mediaTestResultLabel_->setText( "Saved." );
    refreshMediaAccountList();
    for( int i = 0; i < mediaAccountList_->count(); ++i ) {
        if( mediaAccountList_->item( i )->text() == accountId ) {
            mediaAccountList_->setCurrentRow( i );
            break;
        }
    }
}

void SOptionsDialog::onMediaRemoveAccount()
{
    QListWidgetItem *cur = mediaAccountList_->currentItem();
    if( !cur ) return;
    SApplication::app().mediaAccounts()->removeAccount( cur->text() );
    refreshMediaAccountList();
    if( mediaAccountList_->currentItem() == nullptr ) clearMediaForm();
}

void SOptionsDialog::onMediaTestConnection()
{
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    mediaTestResultLabel_->setText( "Testing..." );
    QApplication::processEvents();   // paint "Testing..." before the (bounded) blocking call
    // testAccountConnection(), never testConnection(): the field below is
    // EMPTY after a Save and after picking an account out of the list (the
    // secret is never read back into the UI -- shell CONTRACT inv. 39), so
    // the plain call would send an empty password and report a 401 for an
    // account that browses fine. Empty box + unchanged url/user = test the
    // SAVED credential.
    const QString result = mgr->testAccountConnection( mediaAccountId_->text().trimmed(),
                                                       mediaUrl_->text().trimmed(),
                                                       mediaUser_->text(),
                                                       mediaPassword_->text() );
    mediaTestResultLabel_->setText( result );
}

QString SOptionsDialog::describeMediaPage() const
{
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    QStringList lines;
    // Not really a MEDIA-page fact, but the house pattern for asserting
    // ANYTHING about this dialog off screen is "match a describe*() string"
    // (assert-media-options / assert-midi-options), and this is the
    // cheapest way to make AC-b1's remembered-page round trip assertable
    // without a new verb: `currentPageName()` is already a public accessor.
    lines << QString( "page=%1" ).arg( currentPageName() );
    lines << QString( "backend=%1 canRemember=%2" )
                 .arg( mgr->secretBackendName() )
                 .arg( mgr->canRememberPasswords() ? "yes" : "no" );
    lines << QString( "reason=%1" ).arg( mgr->backendDescription() );
    const QStringList ids = mgr->accountIds();
    lines << QString( "accounts=%1" ).arg( ids.size() );
    for( int i = 0; i < ids.size(); ++i ) {
        const SMediaAccountInfo info = mgr->account( ids.at( i ) );
        QString passwordWord;
        switch( info.passwordStatus ) {
        case SMediaPasswordStatus::Set:            passwordWord = "set"; break;
        case SMediaPasswordStatus::Unset:          passwordWord = "unset"; break;
        case SMediaPasswordStatus::Undecryptable:  passwordWord = "undecryptable"; break;
        }
        // Built by concatenation, not QString(...).arg(...): `accountId`
        // appears twice (acct[]=<id> and source=nextcloud:<id>), and a
        // repeated %n placeholder is exactly the kind of thing arg() chains
        // get subtly wrong.
        lines << QStringLiteral( "  acct[%1]=" ).arg( i ) + info.accountId
                     + QStringLiteral( " url=" ) + info.url
                     + QStringLiteral( " user=" ) + info.user
                     + QStringLiteral( " password=" ) + passwordWord
                     + QStringLiteral( " source=nextcloud:" ) + info.accountId;
    }
    lines << QString( "selected=%1" )
                 .arg( mediaAccountList_->currentItem()
                           ? mediaAccountList_->currentItem()->text() : QString() );
    lines << QString( "form.remember=%1 enabled=%2" )
                 .arg( mediaRemember_->isChecked() ? 1 : 0 )
                 .arg( mediaRemember_->isEnabled() ? 1 : 0 );
    lines << QString( "lastTest=%1" ).arg( mediaTestResultLabel_->text() );
    return lines.join( "\n" );
}

void SOptionsDialog::accept()
{
    apply();
    QDialog::accept();
}

QString SOptionsDialog::currentPageName() const
{
    QTreeWidgetItem *cur = tree_->currentItem();
    return cur ? cur->text( 0 ) : QString();
}

void SOptionsDialog::done( int r )
{
    // "Closing is closing" (AC-b1): QDialog::accept() calls done(Accepted)
    // and the default reject() calls done(Rejected), and Escape / the window
    // close box route through reject() the same way -- so this one override
    // remembers the page whichever of those the user used, with no separate
    // hook needed for each. Written by NAME (see SOpt::OptionsLastPage).
    SSettings::instance().setValue( SOpt::OptionsLastPage, currentPageName() );
    QDialog::done( r );
}
