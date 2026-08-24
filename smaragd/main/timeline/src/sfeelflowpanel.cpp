#include "app/timeline/sfeelflowpanel.h"

#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/sfeelflowbounce.h"
#include "app/objects/track/slearnfeelflowaction.h"
#include "app/objects/track/ssetfeelflowmodeaction.h"
#include "app/objects/track/strackpath.h"
#include "app/shell/sapplication.h"

#include "app/objects/track/strackrndrinline.h"   // feelFlowPalette (M3b strip)

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
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

/**
 * Proposal 40 M3b -- the metric-lab strip: one heatmap row per derived
 * series (SFeelFlowUiData::metrics), every row over the SAME 24-step LUT
 * the arranger band uses (STrackRendererInline::feelFlowPalette), each row
 * spanning the WHOLE analyzed material so the dozen candidates line up
 * column for column. A no-data sentinel (< 0) paints NEUTRAL grey -- a
 * break is not a compliance failure. Pure paint over the panel's cached
 * immutable snapshot: no demand, no store access, no model read.
 */
class SFeelFlowMetricStrip : public QWidget {
public:
    explicit SFeelFlowMetricStrip( QWidget *parent = nullptr )
        : QWidget( parent )
    {
        setFixedHeight( 0 );
    }

    void setData( std::shared_ptr<const SFeelFlowUiData> data )
    {
        if( data == data_ ) return;   // the SAME cached snapshot: no churn
        data_ = std::move( data );
        const int rows =
            data_ && data_->hopFrames != 0 ? (int)data_->metrics.size() : 0;
        setFixedHeight( rows > 0 ? rows * kRowH : 0 );
        update();
    }

    static constexpr int kRowH   = 12;
    static constexpr int kLabelW = 130;

protected:
    void paintEvent( QPaintEvent * ) override
    {
        if( !data_ || data_->hopFrames == 0 || data_->metrics.empty() ) return;
        QPainter p( this );
        const auto &palette = STrackRendererInline::feelFlowPalette();
        const QColor neutral( 64, 64, 64 );
        const int stripW = qMax( 1, width() - kLabelW );

        QFont f = p.font();
        f.setPixelSize( 9 );
        p.setFont( f );

        for( int r = 0; r < (int)data_->metrics.size(); r++ ) {
            const twGrooveMetricSeries &s = data_->metrics[(size_t)r];
            const int y = r * kRowH;
            p.setPen( QColor( 200, 200, 200 ) );
            p.drawText( QRect( 0, y, kLabelW - 4, kRowH ),
                        Qt::AlignVCenter | Qt::AlignLeft,
                        QString::fromStdString( s.id ) );
            const size_t nHops = s.value.size();
            if( nHops == 0 ) continue;
            for( int x = 0; x < stripW; x++ ) {
                const size_t hop = (size_t)( (double)x * (double)nHops / stripW );
                const float v = s.value[qMin( hop, nHops - 1 )];
                const QColor c =
                    v < 0.0f
                        ? neutral
                        : QColor::fromRgba(
                              palette[STrackRendererInline::feelFlowPaletteIndex( v )] );
                p.setPen( c );
                p.drawLine( kLabelW + x, y + 1, kLabelW + x, y + kRowH - 2 );
            }
        }
    }

private:
    std::shared_ptr<const SFeelFlowUiData> data_;
};

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

    // Proposal 40 M3b: the metric lab -- band selector + stacked strip.
    QHBoxLayout *bandRow = new QHBoxLayout();
    bandRow->addWidget( new QLabel( tr( "Band metric" ) ) );
    bandCombo_ = new QComboBox();
    bandCombo_->addItem( QStringLiteral( "compliance" ) );
    bandRow->addWidget( bandCombo_, 1 );
    layout->addLayout( bandRow );
    metricStrip_ = new SFeelFlowMetricStrip();
    layout->addWidget( metricStrip_ );

    connect( bandCombo_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &SFeelFlowPanel::onBandMetricChanged );
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

void SFeelFlowPanel::onBandMetricChanged( int index )
{
    if( applyingExternal_ || !track_ || !bandCombo_ ) return;
    if( index < 0 ) return;
    // A view preference, not an edit: a plain setter, never an action (the
    // same standing as feel-flow-analyze). The setter repaints the arranger
    // through the captureRevalidated funnel itself.
    track_->setFeelFlowBandMetricId( bandCombo_->itemText( index ).toStdString() );
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

    // M3b: the metric lab. The strip shows the snapshot as-is; the combo is
    // rebuilt only when the series SET changed (a snapshot swap), and its
    // selection always mirrors the track's runtime choice.
    if( metricStrip_ )
        metricStrip_->setData(
            data && !track_->feelFlowStale() ? data
                                             : std::shared_ptr<const SFeelFlowUiData>() );
    if( bandCombo_ ) {
        applyingExternal_ = true;
        const bool haveMetrics = data && !data->metrics.empty();
        const int wantCount = haveMetrics ? (int)data->metrics.size() : 1;
        bool rebuild = bandCombo_->count() != wantCount;
        if( !rebuild && haveMetrics )
            for( int i = 0; i < wantCount && !rebuild; i++ )
                rebuild = bandCombo_->itemText( i ).toStdString()
                          != data->metrics[(size_t)i].id;
        if( rebuild ) {
            bandCombo_->clear();
            if( haveMetrics )
                for( const twGrooveMetricSeries &s : data->metrics )
                    bandCombo_->addItem( QString::fromStdString( s.id ) );
            else
                bandCombo_->addItem( QStringLiteral( "compliance" ) );
        }
        const QString want =
            QString::fromStdString( track_->feelFlowBandMetricId() );
        int sel = bandCombo_->findText( want );
        if( sel < 0 ) sel = 0;   // stale id: SHOW the fallback the band paints
        bandCombo_->setCurrentIndex( sel );
        applyingExternal_ = false;
    }

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

    // M3b: the metric lab, in the same grammar the gate reads --
    // bandMetric=<id> metrics=<n>, then one metric:<id>=<min>:<max>:<mean>
    // per series (3 decimals), stats over NON-SENTINEL values plus the
    // sentinel-hop count, so a case can bound both the value range and how
    // much of a series is "no data". The SAME arrays the strip and the
    // band paint -- never a second computation.
    out += QStringLiteral( " bandMetric=%1" )
        .arg( QString::fromStdString( track_->feelFlowBandMetricId() ) );
    if( data && !data->metrics.empty() ) {
        out += QStringLiteral( " metrics=%1" ).arg( data->metrics.size() );
        for( const twGrooveMetricSeries &s : data->metrics ) {
            float mn = 1.0f, mx = 0.0f;
            double sum = 0.0;
            size_t n = 0, sentinel = 0;
            for( float v : s.value ) {
                if( v < 0.0f ) { sentinel++; continue; }
                mn = qMin( mn, v );
                mx = qMax( mx, v );
                sum += v;
                n++;
            }
            if( n == 0 ) { mn = 0.0f; mx = 0.0f; }
            out += QStringLiteral( " metric:%1=%2:%3:%4:%5" )
                       .arg( QString::fromStdString( s.id ) )
                       .arg( mn, 0, 'f', 3 )
                       .arg( mx, 0, 'f', 3 )
                       .arg( n > 0 ? sum / n : 0.0, 0, 'f', 3 )
                       .arg( sentinel );
        }
    }

    return out;
}
