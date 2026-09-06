#include "app/timeline/ssendstrip.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "app/shell/sapplication.h"
#include "app/model/sdefaultreset.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/ssendtap.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/mixer/ssendtapactions.h"
#include "app/objects/track/strack.h"

namespace {

SStdMixer *mixerOf( SProject *project )
{
    return project ? dynamic_cast<SStdMixer *>(
                         splacements::rootContainer( project ) )
                   : nullptr;
}

}  // namespace

SSendStrip::SSendStrip( STrack *track, QWidget *parent )
    : QWidget( parent ), track_( track )
{
    layout_ = new QVBoxLayout( this );
    layout_->setContentsMargins( 0, 0, 0, 0 );
    layout_->setSpacing( 2 );
    rebuild();
}

QString SSendStrip::trackSpec() const
{
    SProject *proj = SApplication::app().getCurrentProject();
    SStdMixer *mixer = mixerOf( proj );
    if( !mixer || !track_ ) return QString();
    const QList<int> path = strackpath::pathOf( mixer, track_ );
    if( path.isEmpty() ) return QString();
    return strackpath::pathToString( path );
}

void SSendStrip::rebuild()
{
    // Torn down and rebuilt wholesale on every track switch, exactly as the FX
    // strip is: this class owns nothing long-lived, and the state it shows
    // lives on the track.
    for( const Row &r : rows_ ) {
        delete r.on;
        delete r.level;
        delete r.mode;
    }
    rows_.clear();
    while( QLayoutItem *it = layout_->takeAt( 0 ) ) {
        delete it->widget();
        delete it;
    }

    SProject *proj = SApplication::app().getCurrentProject();
    SStdMixer *mixer = mixerOf( proj );
    if( !mixer || !track_ ) return;

    const QList<STrack *> lanes = mixer->sendLanes();
    if( lanes.isEmpty() ) return;   // no bus, no section — nothing to say

    QGroupBox *box = new QGroupBox( QStringLiteral( "Sends" ), this );
    QVBoxLayout *inner = new QVBoxLayout( box );
    inner->setContentsMargins( 4, 4, 4, 4 );
    inner->setSpacing( 2 );

    for( int i = 0; i < lanes.size(); ++i ) {
        STrack *lane = lanes[i];
        // A LANE MAY NOT SEND TO ITSELF, so its own strip shows every OTHER
        // lane and not a row that could only ever be refused. The verb refuses
        // it too; a control that exists only to be rejected is worse than one
        // that is not offered.
        if( !lane || lane == track_ ) continue;

        const SSendTap *tap = track_->sendTap( lane->getSName() );

        QWidget *rowW = new QWidget( box );
        QHBoxLayout *rl = new QHBoxLayout( rowW );
        rl->setContentsMargins( 0, 0, 0, 0 );
        rl->setSpacing( 4 );

        Row r;
        r.lane = lane->getSName();

        r.on = new QCheckBox( rowW );
        r.on->setChecked( tap && tap->enabled );
        r.on->setToolTip( QStringLiteral(
            "Feed this track into '%1'. Unticking DISABLES the send and keeps "
            "its level and pre/post choice." ).arg( r.lane ) );
        rl->addWidget( r.on );

        QLabel *name = new QLabel( r.lane, rowW );
        name->setMinimumWidth( 60 );
        rl->addWidget( name, 1 );

        r.level = new QDoubleSpinBox( rowW );
        r.level->setRange( -60.0, 12.0 );
        r.level->setDecimals( 1 );
        r.level->setSingleStep( 0.5 );
        r.level->setSuffix( QStringLiteral( " dB" ) );
        r.level->setValue( tap ? tap->levelDb : 0.0 );
        r.level->setEnabled( tap != nullptr );
        rl->addWidget( r.level );

        r.mode = new QComboBox( rowW );
        r.mode->addItem( QStringLiteral( "post" ) );
        r.mode->addItem( QStringLiteral( "pre" ) );
        r.mode->setCurrentIndex( tap && tap->preFader ? 1 : 0 );
        r.mode->setEnabled( tap != nullptr );
        r.mode->setToolTip( QStringLiteral(
            "post: after this track's fader. pre: after its inserts, before "
            "the fader. A muted track feeds neither." ) );
        rl->addWidget( r.mode );

        const int idx = rows_.size();
        connect( r.on, &QCheckBox::toggled, this,
                 [this, idx]( bool on ) { onToggled( idx, on ); } );
        connect( r.level,
                 QOverload<double>::of( &QDoubleSpinBox::valueChanged ), this,
                 [this, idx]( double v ) { onLevelChanged( idx, v ); } );
        connect( r.mode, QOverload<int>::of( &QComboBox::currentIndexChanged ),
                 this, [this, idx]( int v ) { onModeChanged( idx, v ); } );

        // 0 dB on a double-click, the detail pane's convention
        // (sdefaultreset). A send level has an obvious default; unlike a
        // clip's start time, offering one restores a number rather than
        // inventing one.
        sdefaultreset::onDoubleClick( r.level, [this, idx]() {
            if( idx < rows_.size() && rows_[idx].level )
                rows_[idx].level->setValue( 0.0 );
        } );

        inner->addWidget( rowW );
        rows_.append( r );
    }

    layout_->addWidget( box );
}

void SSendStrip::onToggled( int row, bool on )
{
    if( updating_ || row < 0 || row >= rows_.size() ) return;
    const QString spec = trackSpec();
    if( spec.isEmpty() ) return;
    const Row &r = rows_[row];

    if( on ) {
        // Absent -> create at what the controls currently show. Present but
        // disabled -> just re-enable, so its level and mode come back.
        if( track_ && track_->sendTap( r.lane ) ) {
            auto *a = new SSetSendAction( spec, r.lane );
            a->setEnabled( true );
            SApplication::app().submitAction( a );
        } else {
            SApplication::app().submitAction( new SAddSendAction(
                spec, r.lane, r.level ? r.level->value() : 0.0,
                r.mode && r.mode->currentIndex() == 1, true ) );
        }
    } else {
        auto *a = new SSetSendAction( spec, r.lane );
        a->setEnabled( false );
        SApplication::app().submitAction( a );
    }

    updating_ = true;
    if( rows_[row].level ) rows_[row].level->setEnabled( track_ && track_->sendTap( r.lane ) );
    if( rows_[row].mode )  rows_[row].mode->setEnabled( track_ && track_->sendTap( r.lane ) );
    updating_ = false;
}

void SSendStrip::onLevelChanged( int row, double db )
{
    if( updating_ || row < 0 || row >= rows_.size() ) return;
    const QString spec = trackSpec();
    if( spec.isEmpty() || !track_ ) return;
    const Row &r = rows_[row];
    if( !track_->sendTap( r.lane ) ) return;   // nothing to level yet
    auto *a = new SSetSendAction( spec, r.lane );
    a->setLevelDb( db );
    SApplication::app().submitAction( a );
}

void SSendStrip::onModeChanged( int row, int index )
{
    if( updating_ || row < 0 || row >= rows_.size() ) return;
    const QString spec = trackSpec();
    if( spec.isEmpty() || !track_ ) return;
    const Row &r = rows_[row];
    if( !track_->sendTap( r.lane ) ) return;
    auto *a = new SSetSendAction( spec, r.lane );
    a->setPreFader( index == 1 );
    SApplication::app().submitAction( a );
}

QString SSendStrip::describe() const
{
    QStringList out;
    for( const Row &r : rows_ ) {
        const SSendTap *tap = track_ ? track_->sendTap( r.lane ) : nullptr;
        out << QStringLiteral( "lane=%1|on=%2|level=%3|pre=%4|enabled=%5" )
                   .arg( r.lane )
                   .arg( r.on && r.on->isChecked() ? 1 : 0 )
                   .arg( r.level ? r.level->value() : 0.0 )
                   .arg( r.mode && r.mode->currentIndex() == 1 ? 1 : 0 )
                   .arg( tap && tap->enabled ? 1 : 0 );
    }
    return out.join( QLatin1Char( ';' ) );
}

bool SSendStrip::driveControl( const QString &laneName, const QString &control,
                               double value )
{
    for( int i = 0; i < rows_.size(); ++i ) {
        if( rows_[i].lane != laneName ) continue;
        // The REAL controls, so the signal/slot wiring is what is exercised
        // rather than the handler being called directly.
        if( control == QLatin1String( "on" ) && rows_[i].on ) {
            rows_[i].on->setChecked( value != 0.0 );
            return true;
        }
        if( control == QLatin1String( "level" ) && rows_[i].level ) {
            rows_[i].level->setValue( value );
            return true;
        }
        if( control == QLatin1String( "pre" ) && rows_[i].mode ) {
            rows_[i].mode->setCurrentIndex( value != 0.0 ? 1 : 0 );
            return true;
        }
        return false;
    }
    return false;
}
