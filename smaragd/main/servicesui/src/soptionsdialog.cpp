#include "app/servicesui/soptionsdialog.h"
#include "app/servicesui/soptions.h"
#include "app/shell/ssettings.h"
#include "app/shell/sapplication.h"
#include "tw/playback/twspeaker.h"
#include "tw/devices/audio_backend.h"
#include "tw/devices/audio_input.h"

#include <QTreeWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

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
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Mouse navigation" ) ) );
    tree_->addTopLevelItem( new QTreeWidgetItem( QStringList( "Audio" ) ) );

    stack_ = new QStackedWidget;
    stack_->addWidget( buildMousePage() );   // index 0
    stack_->addWidget( buildAudioPage() );   // index 1

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

    zoomToCursor_ = new QCheckBox( "Zoom toward the mouse cursor" );
    invertZoom_   = new QCheckBox( "Invert zoom direction" );
    form->addRow( QString(), zoomToCursor_ );
    form->addRow( QString(), invertZoom_ );

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

void SOptionsDialog::loadMousePage()
{
    SSettings &s = SSettings::instance();
    selectByData( wheelPlain_,     s.value( SOpt::WheelPlain,     SOpt::def( SOpt::WheelPlain ) ) );
    selectByData( wheelShift_,     s.value( SOpt::WheelShift,     SOpt::def( SOpt::WheelShift ) ) );
    selectByData( wheelCtrl_,      s.value( SOpt::WheelCtrl,      SOpt::def( SOpt::WheelCtrl ) ) );
    selectByData( wheelCtrlShift_, s.value( SOpt::WheelCtrlShift, SOpt::def( SOpt::WheelCtrlShift ) ) );
    zoomToCursor_->setChecked( s.value( SOpt::ZoomToCursor, SOpt::def( SOpt::ZoomToCursor ) ).toBool() );
    invertZoom_->setChecked(  s.value( SOpt::InvertZoom,   SOpt::def( SOpt::InvertZoom ) ).toBool() );
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
}

void SOptionsDialog::loadAudioPage()
{
    // Load output devices
    audioDevice_->clear();
    twSpeaker *spk = SApplication::app().getSpeaker();
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

    // Load input devices
    audioInputDevice_->clear();
    audioInputDevice_->addItem( "System default", "default" );
    // TODO: Phase 7: enumerate input devices via AudioInput factory for this platform
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
        if( twSpeaker *spk = SApplication::app().getSpeaker() ) {
            spk->setOutputDevice( id.toStdString() );
        }
        SSettings::instance().setAudioDeviceId( id );
    }

    // Save input device
    QString inId = audioInputDevice_->currentData().toString();
    if( !inId.isEmpty() ) {
        SSettings::instance().setAudioInputDeviceId( inId );
    }

    // Apply buffer size change (if supported)
    if( twSpeaker *spk = SApplication::app().getSpeaker() ) {
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
}

void SOptionsDialog::accept()
{
    apply();
    QDialog::accept();
}
