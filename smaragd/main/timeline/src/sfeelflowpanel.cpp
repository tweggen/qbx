#include "app/timeline/sfeelflowpanel.h"

#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/sfeelflowbounce.h"
#include "app/objects/track/slearnfeelflowaction.h"
#include "app/objects/track/ssetfeelflowmodeaction.h"
#include "app/objects/track/strackpath.h"
#include "app/shell/sapplication.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QList<int> trackPathOf( STrack *track )
{
    SProject *proj = SApplication::app().getCurrentProject();
    SObject *root = splacements::rootContainer( proj );
    return root ? strackpath::pathOf( root, track ) : QList<int>();
}

} // namespace

SFeelFlowPanel::SFeelFlowPanel( STrack *track, QWidget *parent )
    : QWidget( parent ), track_( track )
{
    QVBoxLayout *layout = new QVBoxLayout( this );
    layout->setContentsMargins( 4, 4, 4, 4 );
    layout->setSpacing( 2 );

    QHBoxLayout *headerRow = new QHBoxLayout();
    headerRow->addWidget( new QLabel( tr( "Feel Flow" ) ) );
    stateLabel_ = new QLabel( QStringLiteral( "never" ) );
    headerRow->addWidget( stateLabel_ );
    headerRow->addStretch( 1 );
    modeCombo_ = new QComboBox();
    modeCombo_->addItem( tr( "Adaptive" ) );   // index 0 == FeelFlowMode::Adaptive
    modeCombo_->addItem( tr( "Trained" ) );    // index 1 == FeelFlowMode::Trained
    headerRow->addWidget( modeCombo_ );
    layout->addLayout( headerRow );

    QHBoxLayout *buttonRow = new QHBoxLayout();
    analyzeButton_ = new QPushButton( tr( "Analyze" ) );
    buttonRow->addWidget( analyzeButton_ );
    learnButton_ = new QPushButton( tr( "Learn from selection" ) );
    buttonRow->addWidget( learnButton_ );
    buttonRow->addStretch( 1 );
    layout->addLayout( buttonRow );

    complianceLabel_ = new QLabel();
    layout->addWidget( complianceLabel_ );
    unitsLabel_ = new QLabel();
    unitsLabel_->setWordWrap( true );
    layout->addWidget( unitsLabel_ );
    tensionLabel_ = new QLabel();
    tensionLabel_->setWordWrap( true );
    layout->addWidget( tensionLabel_ );

    connect( analyzeButton_, &QPushButton::clicked,
             this, &SFeelFlowPanel::onAnalyzeClicked );
    connect( learnButton_, &QPushButton::clicked,
             this, &SFeelFlowPanel::onLearnClicked );
    connect( modeCombo_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &SFeelFlowPanel::onModeChanged );
    connect( &SApplication::app(), &SApplication::meterTick,
             this, &SFeelFlowPanel::onMeterTick );

    refresh();
}

SFeelFlowPanel::~SFeelFlowPanel() = default;

void SFeelFlowPanel::onAnalyzeClicked()
{
    if( !track_ ) return;
    // The SAME call feel-flow-analyze target="track" makes (AC 1) --
    // non-undoable, scheduling a background bounce+analysis is not an edit
    // to the arrangement.
    track_->startFeelFlowBounce();
    refresh();
}

void SFeelFlowPanel::onLearnClicked()
{
    if( !track_ ) return;
    SProject *proj = SApplication::app().getCurrentProject();
    if( !proj ) return;
    SApplication::app().submitAction(
        new SLearnFeelFlowAction( trackPathOf( track_ ) ) );
    refresh();
}

void SFeelFlowPanel::onModeChanged( int index )
{
    if( applyingExternal_ || !track_ ) return;
    const STrack::FeelFlowMode mode =
        index == 1 ? STrack::FeelFlowMode::Trained : STrack::FeelFlowMode::Adaptive;
    if( mode == track_->feelFlowMode() ) return;   // no-op: nothing to submit
    SApplication::app().submitAction(
        new SSetFeelFlowModeAction( trackPathOf( track_ ), mode ) );
    refresh();
}

void SFeelFlowPanel::onMeterTick( offset_t /*pos*/, qint64 /*nowMs*/, bool /*live*/ )
{
    if( !isVisible() ) return;   // hidden dock does no work
    refresh();
}

void SFeelFlowPanel::refresh()
{
    applyingExternal_ = true;
    if( modeCombo_ && track_ )
        modeCombo_->setCurrentIndex(
            track_->feelFlowMode() == STrack::FeelFlowMode::Trained ? 1 : 0 );
    applyingExternal_ = false;

    const QString desc = describe();
    if( stateLabel_ ) {
        // The FIRST token is always "state=<...>" -- see describe()'s grammar.
        const int eq = desc.indexOf( '=' );
        const int sp = desc.indexOf( ' ' );
        stateLabel_->setText( sp > eq && eq >= 0 ? desc.mid( eq + 1, sp - eq - 1 )
                                                 : QStringLiteral( "?" ) );
    }
    if( learnButton_ ) learnButton_->setEnabled( track_ != nullptr );
    if( analyzeButton_ ) analyzeButton_->setEnabled( track_ != nullptr );

    if( !track_ ) {
        if( complianceLabel_ ) complianceLabel_->clear();
        if( unitsLabel_ ) unitsLabel_->clear();
        if( tensionLabel_ ) tensionLabel_->clear();
        return;
    }

    std::shared_ptr<const SFeelFlowUiData> data = track_->feelFlowForUi();
    if( !data || data->hopFrames == 0 || data->compliance.empty() ) {
        if( complianceLabel_ ) complianceLabel_->setText( tr( "compliance: --" ) );
        if( unitsLabel_ ) unitsLabel_->clear();
    } else {
        const offset_t pos = SApplication::app().getGlobalLocatorPos();
        const uint64_t hop = pos >= 0 ? (uint64_t) pos / data->hopFrames : 0;
        if( hop < data->compliance.size() ) {
            if( complianceLabel_ )
                complianceLabel_->setText(
                    tr( "compliance: %1" ).arg( data->compliance[hop], 0, 'f', 3 ) );
            if( unitsLabel_ && data->nUnits > 0 ) {
                QStringList parts;
                for( uint32_t u = 0; u < data->nUnits; u++ ) {
                    const size_t idx = (size_t) hop * data->nUnits + u;
                    const float p = idx < data->perUnitPower.size()
                                       ? data->perUnitPower[idx] : 0.0f;
                    const QString unitName =
                        u < data->unitNames.size()
                            ? QString::fromStdString( data->unitNames[u] )
                            : QStringLiteral( "u%1" ).arg( u );
                    parts << QStringLiteral( "%1:%2" ).arg( unitName )
                                                      .arg( p, 0, 'f', 3 );
                }
                unitsLabel_->setText( tr( "energy: " ) + parts.join( ", " ) );
            }
        }
    }

    if( tensionLabel_ ) {
        if( !data || data->meanSinDeltaPhi.empty() ) {
            tensionLabel_->setText( tr( "counter-tension: --" ) );
        } else {
            // Design section 3.5: the two factors reported SEPARATELY (a
            // loudness confound), per unit, same order as the energy list.
            QStringList lean, drive;
            for( size_t u = 0; u < data->meanSinDeltaPhi.size(); u++ ) {
                const QString unitName =
                    u < data->unitNames.size()
                        ? QString::fromStdString( data->unitNames[u] )
                        : QStringLiteral( "u%1" ).arg( (int) u );
                lean << QStringLiteral( "%1:%2" ).arg( unitName )
                                                 .arg( data->meanSinDeltaPhi[u], 0, 'f', 4 );
                drive << QStringLiteral( "%1:%2" ).arg( unitName )
                                                  .arg( u < data->meanF.size()
                                                          ? data->meanF[u] : 0.0f,
                                                        0, 'f', 4 );
            }
            tensionLabel_->setText( tr( "lean: %1  |  drive: %2" )
                                        .arg( lean.join( ", " ), drive.join( ", " ) ) );
        }
    }
}

QString SFeelFlowPanel::describe() const
{
    if( !track_ ) return QStringLiteral( "state=none" );

    QString state;
    if( track_->isFeelFlowBouncing() )        state = QStringLiteral( "analyzing" );
    else if( !track_->feelFlowHasResult() )    state = QStringLiteral( "never" );
    else if( track_->feelFlowStale() )         state = QStringLiteral( "stale" );
    else                                        state = QStringLiteral( "fresh" );

    QString out = QStringLiteral( "state=%1 mode=%2 trained=%3" )
        .arg( state, STrack::feelFlowModeToString( track_->feelFlowMode() ),
             track_->feelFlowHasTrainedStructure() ? QStringLiteral( "1" )
                                                   : QStringLiteral( "0" ) );

    std::shared_ptr<const SFeelFlowUiData> data = track_->feelFlowForUi();
    if( data && data->hopFrames > 0 && !data->compliance.empty() ) {
        const offset_t pos = SApplication::app().getGlobalLocatorPos();
        const uint64_t hop = pos >= 0 ? (uint64_t) pos / data->hopFrames : 0;
        if( hop < data->compliance.size() ) {
            out += QStringLiteral( " compliance=%1" )
                .arg( data->compliance[hop], 0, 'f', 4 );
            if( data->nUnits > 0 ) {
                QStringList parts;
                for( uint32_t u = 0; u < data->nUnits; u++ ) {
                    const size_t idx = (size_t) hop * data->nUnits + u;
                    const float p = idx < data->perUnitPower.size()
                                       ? data->perUnitPower[idx] : 0.0f;
                    const QString unitName =
                        u < data->unitNames.size()
                            ? QString::fromStdString( data->unitNames[u] )
                            : QStringLiteral( "u%1" ).arg( u );
                    parts << QStringLiteral( "%1:%2" ).arg( unitName )
                                                      .arg( p, 0, 'f', 3 );
                }
                out += QStringLiteral( " units=" ) + parts.join( "," );
            }
        }
    }
    if( data && !data->meanSinDeltaPhi.empty() ) {
        QStringList lean, drive;
        for( size_t u = 0; u < data->meanSinDeltaPhi.size(); u++ ) {
            lean  << QString::number( data->meanSinDeltaPhi[u], 'f', 4 );
            drive << QString::number( u < data->meanF.size() ? data->meanF[u] : 0.0f,
                                      'f', 4 );
        }
        out += QStringLiteral( " lean=" ) + lean.join( "," );
        out += QStringLiteral( " drive=" ) + drive.join( "," );
    }

    return out;
}
