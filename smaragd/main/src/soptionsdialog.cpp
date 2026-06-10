#include "soptionsdialog.h"
#include "soptions.h"
#include "ssettings.h"
#include "sapplication.h"
#include "twspeaker.h"
#include "audio/audio_backend.h"

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

    audioInputDevice_ = new QComboBox;
    form->addRow( "Input device:", audioInputDevice_ );
    form->addRow( new QLabel( "Device selection is used when recording starts." ) );

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
