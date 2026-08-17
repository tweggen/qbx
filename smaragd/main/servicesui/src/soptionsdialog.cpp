#include "app/servicesui/soptionsdialog.h"
#include "app/servicesui/soptions.h"
#include "app/shell/ssettings.h"
#include "app/shell/sapplication.h"
#include "tw/playback/twspeaker.h"
#include "tw/devices/audio_backend.h"
#include "tw/devices/audio_input.h"
#include "tw/devices/midi_output.h"
#include "app/shell/smidioutpump.h"
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
#include <QListWidget>
#include <QListWidgetItem>
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

SOptionsDialog::SOptionsDialog( QWidget *parent )
    : QDialog( parent )
{
    setWindowTitle( "Options" );

    tree_ = new QTreeWidget;
    tree_->setHeaderHidden( true );
    tree_->setFixedWidth( 160 );
    // The tree item order and the stack order must MATCH: currentItemChanged
    // maps by top-level index, nothing else.
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Mouse navigation" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Audio" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "MIDI" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Log" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Plugins" ) ) );

    stack_ = new QStackedWidget;
    stack_->addWidget( buildMousePage() );   // index 0
    stack_->addWidget( buildAudioPage() );   // index 1
    stack_->addWidget( buildMidiPage() );    // index 2
    stack_->addWidget( buildLogPage() );     // index 3
    stack_->addWidget( buildPluginsPage() ); // index 4

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
    loadAudioPage();
    loadMidiPage();

    tree_->setCurrentItem( tree_->topLevelItem( 0 ) );
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

    zoomToCursor_   = new QCheckBox( "Zoom toward the mouse cursor" );
    invertZoom_     = new QCheckBox( "Invert zoom direction" );
    followPlayhead_ = new QCheckBox( "Follow the playhead during playback" );
    form->addRow( QString(), zoomToCursor_ );
    form->addRow( QString(), invertZoom_ );
    form->addRow( QString(), followPlayhead_ );

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

    inputLatencyLabel_ = new QLabel;
    form->addRow( "Input latency:", inputLatencyLabel_ );

    bufferSizeCombo_ = new QComboBox;
    form->addRow( "Buffer size:", bufferSizeCombo_ );
    form->addRow( new QLabel( "Smaller buffer = lower latency but higher CPU load. "
                             "Requires restart of playback to take effect." ) );

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

    box->addWidget( new QLabel( "Input ports (listened to from a later release):" ) );
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
    const std::vector<audio::MidiPortInfo> ins = pump->inputPorts();
    for( const audio::MidiPortInfo &p : ins ) {
        QListWidgetItem *item =
            new QListWidgetItem( QString::fromStdString( p.name ), midiInputs_ );
        item->setData( Qt::UserRole, QString::fromStdString( p.id ) );
        if( wanted.contains( QString::fromStdString( p.id ) ) )
            item->setSelected( true );
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
    s.setValue( SOpt::ZoomToCursor,   zoomToCursor_->isChecked() );
    s.setValue( SOpt::InvertZoom,     invertZoom_->isChecked() );
    s.setValue( SOpt::FollowPlayhead, followPlayhead_->isChecked() );
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
            audioDevice_->addItem( QString::fromStdString( d.name ),
                                   QString::fromStdString( d.id ) );
        }
    }
    QString cur = spk ? QString::fromStdString( spk->outputDevice() )
                      : SSettings::instance().audioDeviceId();
    int i = audioDevice_->findData( cur );
    if( i >= 0 ) audioDevice_->setCurrentIndex( i );

    // Load input devices. REAL since proposal 21 L1b: the same
    // createAudioInput() the live lane uses, so the list is the backend the
    // env selected (a headless run sees the file or null backend, a desktop
    // one sees WASAPI / ALSA / CoreAudio) rather than a hard-coded default.
    // The probe device is opened and closed here and nowhere else; a failure
    // to enumerate leaves just "System default", which is what it did before.
    audioInputDevice_->clear();
    audioInputDevice_->addItem( "System default", "default" );
    {
        std::unique_ptr<audio::AudioInput> probe = audio::createAudioInput();
        if( probe ) {
            for( const audio::AudioInputDeviceInfo &d : probe->listDevices() ) {
                const QString id   = QString::fromStdString( d.id );
                const QString name = QString::fromStdString( d.name );
                if( id.isEmpty() || id == QStringLiteral( "default" ) ) continue;
                audioInputDevice_->addItem(
                    name.isEmpty() ? id
                                   : QStringLiteral( "%1 (%2 ch)" ).arg( name )
                                         .arg( d.channels ),
                    id );
            }
        }
    }
    QString curIn = SSettings::instance().audioInputDeviceId();
    if( curIn.isEmpty() ) curIn = "default";
    int j = audioInputDevice_->findData( curIn );
    if( j >= 0 ) audioInputDevice_->setCurrentIndex( j );

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
}

void SOptionsDialog::applyAudioPage()
{
    // Save output device
    QString id = audioDevice_->currentData().toString();
    if( !id.isEmpty() ) {
        if( auto spk = SApplication::app().getSpeaker() ) {
            spk->setOutputDevice( id.toStdString() );
        }
        SSettings::instance().setAudioDeviceId( id );
    }

    // Save input device
    QString inId = audioInputDevice_->currentData().toString();
    if( !inId.isEmpty() ) {
        const bool moved = SSettings::instance().audioInputDeviceId() != inId;
        SSettings::instance().setAudioInputDeviceId( inId );
        // A DEVICE CHANGE is a live-plan rebuild trigger (proposal 21 design
        // section 3): a track whose trackInput names no device follows this
        // setting, and the monitor has to re-open the input for it. Nothing
        // else in the app is watching this key.
        if( moved ) SApplication::app().liveLanesChanged();
    }

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
        "The result is cached in plugincache.json next to smaragd.ini, keyed on "
        "each module's path, size and modification time — so only plugins that "
        "actually changed are loaded again." );
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

void SOptionsDialog::accept()
{
    apply();
    QDialog::accept();
}
