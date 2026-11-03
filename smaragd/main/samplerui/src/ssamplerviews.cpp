#include "app/samplerui/ssamplerviews.h"

#include "app/actions/saction.h"
#include "app/samplerui/ssamplereditor.h"
#include "app/samplerui/ssamplerzones.h"

#include "tw/sampler/twsamplerprogrammodel.h"

#include <QButtonGroup>
#include <QDial>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using audio::twSamplerProgramModel;
using audio::twSamplerScope;

namespace {

const char *const kResourceMime = "application/x-smaragd-resource";

const twSamplerProgramModel *modelOf( const SSamplerEditor *ed )
{
    SSamplerProgram *p = ed ? ed->program() : nullptr;
    return p ? &p->model() : nullptr;
}

QVector<ssamplerzones::ZoneInfo> zonesOf( const SSamplerEditor *ed )
{
    const twSamplerProgramModel *m = modelOf( ed );
    return m ? ssamplerzones::zones( *m ) : QVector<ssamplerzones::ZoneInfo>();
}

QString idList( const QVector<std::uint32_t> &ids )
{
    QStringList parts;
    for( std::uint32_t id : ids ) parts << QString::number( id );
    return parts.join( QLatin1Char( ';' ) );
}

bool isBlackKey( int key )
{
    switch( key % 12 ) {
    case 1: case 3: case 6: case 8: case 10: return true;
    default: return false;
    }
}

// Submit select-zones replacing the selection, from a view's own press.
void selectZones( SSamplerEditor *ed, const QVector<std::uint32_t> &ids )
{
    QString why;
    const QString xml = QStringLiteral( "<select-zones zones=\"%1\" mode=\"replace\"/>" )
                            .arg( idList( ids ) );
    if( !ed->submitVerbXml( xml, 0, &why ) ) ed->setStatus( why );
}

}  // namespace

// ============================================================== base =====

SSamplerViewBase::SSamplerViewBase( SSamplerEditor *editor, QWidget *parent )
    : QWidget( parent ), editor_( editor )
{
    setAcceptDrops( true );
}

void SSamplerViewBase::dragEnterEvent( QDragEnterEvent *e )
{
    if( e->mimeData()->hasFormat( QLatin1String( kResourceMime ) ) ) e->acceptProposedAction();
}

void SSamplerViewBase::dragMoveEvent( QDragMoveEvent *e )
{
    int key = -1;
    std::uint32_t zone = 0;
    if( e->mimeData()->hasFormat( QLatin1String( kResourceMime ) ) &&
        dropTargetAt( e->position().toPoint(), &key, &zone ) )
        e->acceptProposedAction();
    else
        e->ignore();
}

void SSamplerViewBase::dropEvent( QDropEvent *e )
{
    lastDropOk_ = false;
    lastDropWhy_.clear();
    if( !e->mimeData()->hasFormat( QLatin1String( kResourceMime ) ) ) {
        lastDropWhy_ = QStringLiteral( "not a resource drop" );
        return;
    }
    const QString payload =
        QString::fromUtf8( e->mimeData()->data( QLatin1String( kResourceMime ) ) );
    int key = -1;
    std::uint32_t zone = 0;
    if( !dropTargetAt( e->position().toPoint(), &key, &zone ) ) {
        lastDropWhy_ = QStringLiteral( "nothing to drop on there" );
        editor_->setStatus( lastDropWhy_ );
        return;
    }
    e->acceptProposedAction();
    lastDropOk_ = editor_->dropOn( payload, key, zone, &lastDropWhy_ );
    if( !lastDropOk_ ) editor_->setStatus( lastDropWhy_ );
}

// =============================================================== key =====

SSamplerKeyView::SSamplerKeyView( SSamplerEditor *editor, QWidget *parent )
    : SSamplerViewBase( editor, parent )
{
    setFixedSize( SSamplerEditor::keyViewSize() );
}

QPoint SSamplerKeyView::keyCentre( int key )
{
    return QPoint( key * kKeyWidth + kKeyWidth / 2, kKeysTop + 40 );
}

int SSamplerKeyView::keyAt( const QPoint &pos )
{
    const int k = pos.x() / kKeyWidth;
    return ( pos.x() >= 0 && k >= 0 && k <= 127 ) ? k : -1;
}

void SSamplerKeyView::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    const QPalette &pal = palette();
    p.fillRect( rect(), pal.color( QPalette::Base ) );

    // Octave labels: C of every octave.
    p.setPen( pal.color( QPalette::Text ) );
    QFont f = font();
    f.setPointSizeF( std::max( 6.0, f.pointSizeF() * 0.8 ) );
    p.setFont( f );
    for( int k = 0; k <= 127; k += 12 )
        p.drawText( QRect( k * kKeyWidth, 0, 12 * kKeyWidth, kZoneTop ),
                    Qt::AlignLeft | Qt::AlignVCenter, ssamplerzones::keyName( k ) );

    // The zones, one bar per zone, stacked into rows.
    const QVector<ssamplerzones::ZoneInfo> zones = zonesOf( editor_ );
    const QVector<std::uint32_t> sel = editor_->selectedZones();
    for( int i = 0; i < zones.size(); ++i ) {
        const ssamplerzones::ZoneInfo &z = zones[i];
        const int row = i % kZoneRows;
        const QRect r( z.keyLo * kKeyWidth, kZoneTop + row * kZoneRow,
                       ( z.keyHi - z.keyLo + 1 ) * kKeyWidth - 1, kZoneRow - 2 );
        const bool selected = sel.contains( z.id );
        p.fillRect( r, selected ? pal.color( QPalette::Highlight ) : pal.color( QPalette::Mid ) );
        // The root key, a tick.
        p.setPen( selected ? pal.color( QPalette::HighlightedText ) : pal.color( QPalette::Text ) );
        const int rx = z.rootKey * kKeyWidth + kKeyWidth / 2;
        if( z.rootKey >= z.keyLo && z.rootKey <= z.keyHi ) p.drawLine( rx, r.top(), rx, r.bottom() );
    }

    // The keyboard.
    const QSet<int> held = editor_->heldKeys();
    const int keysH = height() - kKeysTop;
    for( int k = 0; k <= 127; ++k ) {
        const QRect r( k * kKeyWidth, kKeysTop, kKeyWidth - 1, keysH - 1 );
        QColor c = isBlackKey( k ) ? pal.color( QPalette::Text ) : pal.color( QPalette::Window );
        if( held.contains( k ) ) c = pal.color( QPalette::Highlight );
        p.fillRect( r, c );
        if( k % 12 == 0 ) {
            p.setPen( pal.color( QPalette::Dark ) );
            p.drawLine( r.left(), kKeysTop, r.left(), height() );
        }
    }
}

void SSamplerKeyView::mousePressEvent( QMouseEvent *e )
{
    const int key = keyAt( e->position().toPoint() );
    if( key < 0 || e->button() != Qt::LeftButton ) return;
    const twSamplerProgramModel *m = modelOf( editor_ );
    QVector<std::uint32_t> ids;
    if( m )
        for( std::uint32_t id : m->zonesCoveringKey( key ) ) ids.append( id );
    selectZones( editor_, ids );
}

bool SSamplerKeyView::dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const
{
    const int k = keyAt( pos );
    if( k < 0 ) return false;
    *key  = k;
    *zone = 0;
    return true;
}

// =============================================================== pad =====

SSamplerPadView::SSamplerPadView( SSamplerEditor *editor, QWidget *parent )
    : SSamplerViewBase( editor, parent )
{
    setFixedSize( SSamplerEditor::padViewSize() );
    banks_ = new QButtonGroup( this );
    banks_->setExclusive( true );
    for( int b = 0; b < SSamplerProgram::kPadBanks; ++b ) {
        auto *btn = new QPushButton( QString( QChar( 'A' + b ) ), this );
        btn->setObjectName( QStringLiteral( "bank%1" ).arg( b ) );
        btn->setCheckable( true );
        btn->setToolTip( tr( "Bank %1" ).arg( QChar( 'A' + b ) ) );
        btn->setGeometry( kGap + b * 64, kGap, 56, 30 );
        banks_->addButton( btn, b );
        bankButton_[b] = btn;
        connect( btn, &QPushButton::clicked, this, [this, b]() {
            SSamplerProgram *prog = editor_->program();
            const int pad = prog ? prog->pad() : 0;
            // The pad stays where it is when it exists in the new bank; a
            // bank-5 switch from pad 12..15 (n 92..95, keyless without a
            // row) falls back to pad 0 rather than being refused.
            const bool keeps = prog ? prog->padExists( b, pad ) : ( b < 5 || pad < 12 );
            QString why;
            if( !editor_->submitVerbXml(
                    QStringLiteral( "<set-sampler-view padBank=\"%1\" pad=\"%2\"/>" )
                        .arg( b ).arg( keeps ? pad : 0 ),
                    0, &why ) )
                editor_->setStatus( why );
        } );
    }
}

QRect SSamplerPadView::cellRect( int pad )
{
    const int col = pad % 4;
    const int row = 3 - pad / 4;   // pad 0 bottom left
    return QRect( kGap + col * ( kCell + kGap ), kGridTop + row * ( kCell + kGap ), kCell, kCell );
}

int SSamplerPadView::padAt( const QPoint &pos )
{
    for( int p = 0; p < SSamplerProgram::kPadsPerBank; ++p )
        if( cellRect( p ).contains( pos ) ) return p;
    return -1;
}

bool SSamplerPadView::clickBank( int bank )
{
    if( bank < 0 || bank >= SSamplerProgram::kPadBanks ) return false;
    bankButton_[bank]->click();
    return true;
}

void SSamplerPadView::refreshFromModel()
{
    SSamplerProgram *prog = editor_->program();
    const int bank = prog ? prog->padBank() : 0;
    if( QAbstractButton *b = banks_->button( bank ) ) b->setChecked( true );
    update();
}

void SSamplerPadView::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    const QPalette &pal = palette();
    p.fillRect( rect(), pal.color( QPalette::Window ) );
    SSamplerProgram *prog = editor_->program();
    const twSamplerProgramModel *m = modelOf( editor_ );
    const twSamplerProgramModel empty;
    const twSamplerProgramModel &model = m ? *m : empty;
    const int bank = prog ? prog->padBank() : 0;
    const int selPad = prog ? prog->pad() : 0;
    for( int pad = 0; pad < SSamplerProgram::kPadsPerBank; ++pad ) {
        const QRect r = cellRect( pad );
        const int key = model.padKey( SSamplerProgram::padNumberOf( bank, pad ) );
        const bool selected = ( pad == selPad );
        if( key < 0 ) {
            p.setPen( pal.color( QPalette::Mid ) );
            p.drawRect( r.adjusted( 0, 0, -1, -1 ) );
            continue;
        }
        p.fillRect( r, selected ? pal.color( QPalette::Highlight ) : pal.color( QPalette::Button ) );
        p.setPen( selected ? pal.color( QPalette::HighlightedText ) : pal.color( QPalette::ButtonText ) );
        const int zonesOnKey = (int) model.zonesCoveringKey( key ).size();
        const QString text =
            QStringLiteral( "%1%2\n%3\n%4" )
                .arg( QChar( 'A' + bank ) )
                .arg( pad + 1, 2, 10, QLatin1Char( '0' ) )
                .arg( ssamplerzones::keyName( key ) )
                .arg( zonesOnKey == 0 ? tr( "empty" )
                                      : ( zonesOnKey == 1 ? tr( "1 zone" )
                                                          : tr( "%1 zones" ).arg( zonesOnKey ) ) );
        p.drawText( r, Qt::AlignCenter, text );
        if( editor_->heldKeys().contains( key ) ) {
            QPen pen( pal.color( QPalette::Highlight ) );
            pen.setWidth( 3 );
            p.setPen( pen );
            p.drawRect( r.adjusted( 1, 1, -2, -2 ) );
        }
    }
}

void SSamplerPadView::mousePressEvent( QMouseEvent *e )
{
    if( e->button() != Qt::LeftButton ) return;
    const int pad = padAt( e->position().toPoint() );
    if( pad < 0 ) return;
    SSamplerProgram *prog = editor_->program();
    const int bank = prog ? prog->padBank() : 0;
    QString why;
    if( !editor_->submitVerbXml( QStringLiteral( "<set-sampler-view padBank=\"%1\" pad=\"%2\"/>" )
                                     .arg( bank ).arg( pad ),
                                 0, &why ) )
        editor_->setStatus( why );
}

bool SSamplerPadView::dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const
{
    const int pad = padAt( pos );
    if( pad < 0 ) return false;
    SSamplerProgram *prog = editor_->program();
    const twSamplerProgramModel empty;
    const twSamplerProgramModel &model = prog ? prog->model() : empty;
    const int k = model.padKey( SSamplerProgram::padNumberOf( prog ? prog->padBank() : 0, pad ) );
    if( k < 0 ) return false;
    *key  = k;
    *zone = 0;
    return true;
}

// =============================================================== map =====

SSamplerMapView::SSamplerMapView( SSamplerEditor *editor, QWidget *parent )
    : SSamplerViewBase( editor, parent )
{
    setFixedSize( SSamplerEditor::mapViewSize() );
}

QPoint SSamplerMapView::pointFor( int key, int vel )
{
    return QPoint( kLeft + key * kKeyW + kKeyW / 2, kTop + ( 127 - vel ) * kVelH + kVelH / 2 );
}

bool SSamplerMapView::cellAt( const QPoint &pos, int *key, int *vel )
{
    const int x = pos.x() - kLeft;
    const int y = pos.y() - kTop;
    if( x < 0 || y < 0 ) return false;
    const int k = x / kKeyW;
    const int v = 127 - y / kVelH;
    if( k > 127 || v < 0 ) return false;
    *key = k;
    *vel = v;
    return true;
}

void SSamplerMapView::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    const QPalette &pal = palette();
    p.fillRect( rect(), pal.color( QPalette::Window ) );
    const QRect grid( kLeft, kTop, 128 * kKeyW, 128 * kVelH );
    p.fillRect( grid, pal.color( QPalette::Base ) );

    // Axes: octaves along the bottom, velocity 0/64/127 on the left.
    p.setPen( pal.color( QPalette::Text ) );
    QFont f = font();
    f.setPointSizeF( std::max( 6.0, f.pointSizeF() * 0.8 ) );
    p.setFont( f );
    for( int k = 0; k <= 127; k += 12 ) {
        p.drawText( QRect( kLeft + k * kKeyW, grid.bottom() + 2, 12 * kKeyW, 20 ),
                    Qt::AlignLeft | Qt::AlignTop, ssamplerzones::keyName( k ) );
        p.setPen( pal.color( QPalette::Midlight ) );
        p.drawLine( kLeft + k * kKeyW, kTop, kLeft + k * kKeyW, grid.bottom() );
        p.setPen( pal.color( QPalette::Text ) );
    }
    for( int v : { 0, 64, 127 } )
        p.drawText( QRect( 0, pointFor( 0, v ).y() - 8, kLeft - 4, 16 ),
                    Qt::AlignRight | Qt::AlignVCenter, QString::number( v ) );

    const QVector<ssamplerzones::ZoneInfo> zones = zonesOf( editor_ );
    const QVector<std::uint32_t> sel = editor_->selectedZones();
    for( const ssamplerzones::ZoneInfo &z : zones ) {
        const QRect r( kLeft + z.keyLo * kKeyW, kTop + ( 127 - z.velHi ) * kVelH,
                       ( z.keyHi - z.keyLo + 1 ) * kKeyW, ( z.velHi - z.velLo + 1 ) * kVelH );
        const bool selected = sel.contains( z.id );
        QColor fill = selected ? pal.color( QPalette::Highlight ) : pal.color( QPalette::Mid );
        fill.setAlpha( 150 );
        p.fillRect( r, fill );
        p.setPen( pal.color( QPalette::Text ) );
        p.drawRect( r.adjusted( 0, 0, -1, -1 ) );
        // THE VELOCITY GAPS (DESIGN §2): hatched, outlined in the bright text
        // colour so a gap reads on any theme.
        for( const auto &g : z.gaps ) {
            const QRect gr( r.left(), kTop + ( 127 - g.second ) * kVelH, r.width(),
                            ( g.second - g.first + 1 ) * kVelH );
            p.fillRect( gr, QBrush( pal.color( QPalette::BrightText ), Qt::BDiagPattern ) );
            p.setPen( pal.color( QPalette::BrightText ) );
            p.drawRect( gr.adjusted( 0, 0, -1, -1 ) );
        }
        p.setPen( selected ? pal.color( QPalette::HighlightedText ) : pal.color( QPalette::Text ) );
        p.drawText( r.adjusted( 2, 1, -1, -1 ), Qt::AlignLeft | Qt::AlignTop, QString::number( z.id ) );
    }
}

void SSamplerMapView::mousePressEvent( QMouseEvent *e )
{
    if( e->button() != Qt::LeftButton ) return;
    int key = 0, vel = 0;
    if( !cellAt( e->position().toPoint(), &key, &vel ) ) return;
    QVector<std::uint32_t> ids;
    for( const ssamplerzones::ZoneInfo &z : zonesOf( editor_ ) )
        if( z.covers( key, vel ) ) ids.append( z.id );
    selectZones( editor_, ids );
}

bool SSamplerMapView::dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const
{
    int k = 0, v = 0;
    if( !cellAt( pos, &k, &v ) ) return false;
    // On a zone rectangle: that zone (the first that holds the point, in
    // program order); on empty grid: the key, as the Key view takes it.
    for( const ssamplerzones::ZoneInfo &z : zonesOf( editor_ ) )
        if( z.covers( k, v ) ) {
            *key  = -1;
            *zone = z.id;
            return true;
        }
    *key  = k;
    *zone = 0;
    return true;
}

// ============================================================== zone =====

namespace {

struct KnobSpec {
    const char *attr;
    const char *label;
};

const KnobSpec kAmp[]      = { { "level", "Level" }, { "pan", "Pan" }, { "tune", "Tune" } };
const KnobSpec kDrive[]    = { { "drive", "Drive" }, { "driveMix", "Mix" } };
const KnobSpec kFilter[]   = { { "cutoff", "Cutoff" }, { "resonance", "Reso" },
                               { "filterEnvAmount", "Env amt" }, { "filterKeyTrack", "Key trk" } };
const KnobSpec kEnvelope[] = { { "attack", "Attack" }, { "hold", "Hold" }, { "decay", "Decay" },
                               { "sustain", "Sustain" }, { "release", "Release" } };
const KnobSpec kLayer[]    = { { "level", "Level" }, { "tune", "Tune" }, { "pan", "Pan" } };

constexpr int kTicks = 1000;

QString formatValue( double v, bool integer )
{
    if( integer ) return QString::number( (long long) std::llround( v ) );
    return QString::number( v, 'g', 4 );
}

}  // namespace

SSamplerZoneView::SSamplerZoneView( SSamplerEditor *editor, QWidget *parent )
    : SSamplerViewBase( editor, parent )
{
    setFixedSize( SSamplerEditor::zoneViewSize() );
    auto *outer = new QVBoxLayout( this );
    outer->setContentsMargins( 8, 6, 8, 6 );
    outer->setSpacing( 4 );
    header_ = new QLabel( this );
    header_->setObjectName( QStringLiteral( "zoneHeader" ) );
    outer->addWidget( header_ );
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing( 6 );
    grid->setVerticalSpacing( 2 );
    outer->addLayout( grid );
    outer->addStretch( 1 );

    auto addKnob = [this, grid]( int row, int col, const QString &id, bool layer, int layerIndex,
                                 const char *attr, const QString &label ) {
        Knob k;
        k.id         = id;
        k.layer      = layer;
        k.layerIndex = layerIndex;
        k.attr       = QString::fromLatin1( attr );
        const twSamplerScope scope = layer ? twSamplerScope::Layer : twSamplerScope::Zone;
        twSamplerProgramModel::paramRange( scope, attr, &k.lo, &k.hi );
        k.integer  = twSamplerProgramModel::paramIsInteger( scope, attr );
        k.logScale = ( k.attr == QLatin1String( "cutoff" ) && k.lo > 0.0 );
        auto *cell = new QWidget( this );
        auto *v    = new QVBoxLayout( cell );
        v->setContentsMargins( 0, 0, 0, 0 );
        v->setSpacing( 0 );
        auto *name = new QLabel( label, cell );
        name->setAlignment( Qt::AlignHCenter );
        k.dial = new QDial( cell );
        k.dial->setObjectName( id );
        k.dial->setFixedSize( 40, 40 );
        k.dial->setRange( 0, ( k.integer && k.hi - k.lo <= kTicks ) ? int( k.hi - k.lo ) : kTicks );
        k.dial->setToolTip( id );
        k.value = new QLabel( cell );
        k.value->setAlignment( Qt::AlignHCenter );
        v->addWidget( name );
        v->addWidget( k.dial, 0, Qt::AlignHCenter );
        v->addWidget( k.value );
        grid->addWidget( cell, row, col );
        const int index = knobs_.size();
        knobs_.append( k );
        connect( k.dial, &QDial::valueChanged, this, [this, index]() { onDial_( index ); } );
        // THE GESTURE (QBX-206): a token minted on press stamps every action of
        // the drag, so set-zone's mergeKey folds the drag into ONE undo step;
        // released, the next drag is a step of its own.
        connect( k.dial, &QDial::sliderPressed, this,
                 [this, index]() { knobs_[index].gesture = SAction::newGesture(); } );
        connect( k.dial, &QDial::sliderReleased, this,
                 [this, index]() { knobs_[index].gesture = 0; } );
    };

    auto rowLabel = [this, grid]( int row, const QString &text ) {
        auto *l = new QLabel( text, this );
        l->setAlignment( Qt::AlignRight | Qt::AlignVCenter );
        grid->addWidget( l, row, 0 );
    };

    int row = 0;
    rowLabel( row, tr( "Amp" ) );
    for( int i = 0; i < 3; ++i )
        addKnob( row, 1 + i, QStringLiteral( "zone.%1" ).arg( kAmp[i].attr ), false, 0, kAmp[i].attr,
                 tr( kAmp[i].label ) );
    ++row;
    rowLabel( row, tr( "Drive" ) );
    for( int i = 0; i < 2; ++i )
        addKnob( row, 1 + i, QStringLiteral( "zone.%1" ).arg( kDrive[i].attr ), false, 0,
                 kDrive[i].attr, tr( kDrive[i].label ) );
    ++row;
    rowLabel( row, tr( "Filter" ) );
    for( int i = 0; i < 4; ++i )
        addKnob( row, 1 + i, QStringLiteral( "zone.%1" ).arg( kFilter[i].attr ), false, 0,
                 kFilter[i].attr, tr( kFilter[i].label ) );
    ++row;
    rowLabel( row, tr( "Envelope" ) );
    for( int i = 0; i < 5; ++i )
        addKnob( row, 1 + i, QStringLiteral( "zone.%1" ).arg( kEnvelope[i].attr ), false, 0,
                 kEnvelope[i].attr, tr( kEnvelope[i].label ) );
    ++row;
    rowLabel( row, tr( "Layers" ) );
    for( int l = 0; l < 4; ++l )
        for( int i = 0; i < 3; ++i )
            addKnob( row, 1 + l * 3 + i, QStringLiteral( "layer%1.%2" ).arg( l + 1 ).arg( kLayer[i].attr ),
                     true, l, kLayer[i].attr,
                     tr( "L%1 %2" ).arg( l + 1 ).arg( tr( kLayer[i].label ) ) );
    refreshFromModel();
}

int SSamplerZoneView::ticksFor_( const Knob &k, double v ) const
{
    const int n = k.dial->maximum();
    if( k.hi <= k.lo || n <= 0 ) return 0;
    v = std::clamp( v, k.lo, k.hi );
    double t;
    if( k.logScale ) t = std::log( v / k.lo ) / std::log( k.hi / k.lo );
    else             t = ( v - k.lo ) / ( k.hi - k.lo );
    return (int) std::lround( t * n );
}

double SSamplerZoneView::valueFor_( const Knob &k, int ticks ) const
{
    const int n = k.dial->maximum();
    const double t = n > 0 ? double( ticks ) / n : 0.0;
    double v = k.logScale ? k.lo * std::pow( k.hi / k.lo, t ) : k.lo + t * ( k.hi - k.lo );
    if( k.integer ) v = std::round( v );
    return v;
}

void SSamplerZoneView::refreshFromModel()
{
    applying_ = true;
    const twSamplerProgramModel *m = modelOf( editor_ );
    const QVector<std::uint32_t> sel = editor_->selectedZones();
    shownZone_ = ( m && !sel.isEmpty() ) ? sel.first() : 0;
    std::vector<std::uint32_t> layers;
    if( shownZone_ ) layers = m->layerIds( shownZone_ );
    if( !shownZone_ ) {
        header_->setText( tr( "No zone selected: hold a key, or click a zone in the Key or Map view." ) );
    } else {
        const ssamplerzones::ZoneInfo *zi = nullptr;
        const QVector<ssamplerzones::ZoneInfo> zones = ssamplerzones::zones( *m );
        for( const auto &z : zones )
            if( z.id == shownZone_ ) zi = &z;
        QString text = tr( "Zone %1" ).arg( shownZone_ );
        if( zi )
            text += tr( "   keys %1 to %2, velocity %3 to %4" )
                        .arg( ssamplerzones::keyName( zi->keyLo ), ssamplerzones::keyName( zi->keyHi ) )
                        .arg( zi->velLo )
                        .arg( zi->velHi );
        if( sel.size() > 1 ) text += tr( "   (%1 zones selected: the knobs set them all)" ).arg( sel.size() );
        header_->setText( text );
    }
    for( Knob &k : knobs_ ) {
        std::uint32_t target = 0;
        if( !k.layer ) target = shownZone_;
        else if( k.layerIndex < (int) layers.size() ) target = layers[k.layerIndex];
        const bool enabled = target != 0;
        k.dial->setEnabled( enabled );
        if( !enabled ) {
            k.value->setText( QStringLiteral( "-" ) );
            continue;
        }
        std::string text;
        m->value( k.layer ? twSamplerScope::Layer : twSamplerScope::Zone, target,
                  k.attr.toStdString(), &text );
        bool ok = false;
        const double v = QString::fromStdString( text ).toDouble( &ok );
        if( !ok ) continue;
        // A dial the hand holds shows the hand (its own echo would fight it).
        if( !k.dial->isSliderDown() ) k.dial->setValue( ticksFor_( k, v ) );
        k.value->setText( formatValue( v, k.integer ) );
    }
    applying_ = false;
    update();
}

void SSamplerZoneView::onDial_( int index )
{
    if( applying_ || index < 0 || index >= knobs_.size() ) return;
    const Knob &k = knobs_[index];
    const twSamplerProgramModel *m = modelOf( editor_ );
    if( !m || !shownZone_ ) return;
    const double v = valueFor_( k, k.dial->value() );
    // `=` forces an ABSOLUTE value (a leading sign would be a relative edit,
    // DESIGN §2.1); QString::number is the C locale.
    const QString value = QStringLiteral( "=" ) +
                          ( k.integer ? QString::number( (long long) std::llround( v ) )
                                      : QString::number( v, 'g', 10 ) );
    QString xml;
    if( !k.layer ) {
        QVector<std::uint32_t> sel = editor_->selectedZones();
        if( sel.isEmpty() ) sel.append( shownZone_ );
        xml = QStringLiteral( "<set-zone zones=\"%1\" %2=\"%3\"/>" ).arg( idList( sel ), k.attr, value );
    } else {
        const std::vector<std::uint32_t> layers = m->layerIds( shownZone_ );
        if( k.layerIndex >= (int) layers.size() ) return;
        xml = QStringLiteral( "<set-layer layer=\"%1\" %2=\"%3\"/>" )
                  .arg( layers[k.layerIndex] )
                  .arg( k.attr, value );
    }
    k.value->setText( formatValue( v, k.integer ) );
    QString why;
    if( !editor_->submitVerbXml( xml, k.gesture, &why ) ) editor_->setStatus( why );
}

QDial *SSamplerZoneView::dialFor( const QString &id ) const
{
    for( const Knob &k : knobs_ )
        if( k.id == id ) return k.dial;
    return nullptr;
}

bool SSamplerZoneView::setKnob( const QString &id, double value )
{
    for( int i = 0; i < knobs_.size(); ++i ) {
        Knob &k = knobs_[i];
        if( k.id != id ) continue;
        if( !k.dial->isEnabled() ) return false;
        const int t = ticksFor_( k, value );
        if( k.dial->value() == t ) onDial_( i );   // setValue would not emit
        else                       k.dial->setValue( t );
        return true;
    }
    return false;
}

QString SSamplerZoneView::describeKnobs() const
{
    QStringList parts;
    for( const Knob &k : knobs_ )
        parts << QStringLiteral( "%1=%2" ).arg( k.id, k.dial->isEnabled() ? k.value->text()
                                                                         : QStringLiteral( "-" ) );
    return parts.join( QLatin1Char( ',' ) );
}

bool SSamplerZoneView::dropTargetAt( const QPoint &, int *key, std::uint32_t *zone ) const
{
    if( !shownZone_ ) return false;
    *key  = -1;
    *zone = shownZone_;
    return true;
}
