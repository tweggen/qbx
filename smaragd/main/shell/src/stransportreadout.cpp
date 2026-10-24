#include "app/shell/stransportreadout.h"

#include <QHBoxLayout>
#include <QLabel>

#include "tw/core/twdomains.h"
#include "tw/events/twtempomap.h"

namespace {

// Ticks from the project start to `frames`, rounded to the NEAREST tick (see
// the header: a snapped bar line is a whole frame, the bar itself usually is
// not, and a floor would land one tick early).
int64_t nearestTicks( offset_t frames, const twTempoMap &map, int sampleRate )
{
    const Fraction exact =
        map.framesToTickLen( (int64_t) frames, sampleRate ).ticks();
    return ( exact + Fraction( (int64_t) 1, (int64_t) 2 ) ).floorToInt();
}

QString clock( offset_t frames, int sampleRate )
{
    const int64_t ms = sampleRate > 0
        ? (int64_t) ( (double) frames * 1000.0 / sampleRate + 0.5 ) : 0;
    return QStringLiteral( "%1:%2.%3" )
        .arg( ms / 60000 )
        .arg( ( ms / 1000 ) % 60, 2, 10, QLatin1Char( '0' ) )
        .arg( ms % 1000, 3, 10, QLatin1Char( '0' ) );
}

}  // namespace

STransportReadout::STransportReadout( QWidget *parent )
    : QWidget( parent )
{
    QHBoxLayout *l = new QHBoxLayout( this );
    l->setContentsMargins( 6, 0, 6, 0 );
    l->setSpacing( 10 );

    pos_ = new QLabel( this );
    pos_->setObjectName( QStringLiteral( "transport_position" ) );
    pos_->setToolTip( tr( "Playhead position" ) );
    l->addWidget( pos_ );

    sel_ = new QLabel( this );
    sel_->setObjectName( QStringLiteral( "transport_selection" ) );
    sel_->setToolTip( tr( "Time selection: start – end (length)" ) );
    l->addWidget( sel_ );

    // Fixed-width digits, so the toolbar does not twitch while the playhead
    // runs.
    QFont f = font();
    f.setFamily( QStringLiteral( "monospace" ) );
    f.setStyleHint( QFont::TypeWriter );
    pos_->setFont( f );
    sel_->setFont( f );

    pos_->setText( QStringLiteral( "▶ —" ) );
    sel_->setText( QStringLiteral( "[ — ]" ) );
}

QString STransportReadout::formatPosition( offset_t frame,
                                           const twTempoMap &map,
                                           int sampleRate, bool barsMode )
{
    if( !barsMode ) return clock( frame, sampleRate );
    const int64_t t       = nearestTicks( frame, map, sampleRate );
    const int64_t bar     = map.barTicks();
    const int64_t beat    = (int64_t) map.ppq() * 4 / map.denominator();
    if( bar <= 0 || beat <= 0 ) return clock( frame, sampleRate );
    return QStringLiteral( "%1.%2.%3" )
        .arg( t / bar + 1 )
        .arg( ( t % bar ) / beat + 1 )
        .arg( t % beat, 3, 10, QLatin1Char( '0' ) );
}

QString STransportReadout::formatLength( offset_t frames,
                                         const twTempoMap &map,
                                         int sampleRate, bool barsMode )
{
    if( !barsMode ) return clock( frames, sampleRate );
    // A LENGTH counts from zero: four bars is 4.0.000, not 5.1.000. Rounded
    // like a position, for the same reason.
    const int64_t t    = nearestTicks( frames, map, sampleRate );
    const int64_t bar  = map.barTicks();
    const int64_t beat = (int64_t) map.ppq() * 4 / map.denominator();
    if( bar <= 0 || beat <= 0 ) return clock( frames, sampleRate );
    return QStringLiteral( "%1.%2.%3" )
        .arg( t / bar )
        .arg( ( t % bar ) / beat )
        .arg( t % beat, 3, 10, QLatin1Char( '0' ) );
}

void STransportReadout::setState( offset_t position, bool hasRange,
                                  offset_t rangeStart, offset_t rangeEnd,
                                  const twTempoMap &map, int sampleRate,
                                  bool barsMode )
{
    bars_ = barsMode;
    selFrames_ = ( hasRange && rangeEnd > rangeStart )
        ? QStringLiteral( "%1-%2" ).arg( (qulonglong) rangeStart )
                                   .arg( (qulonglong) rangeEnd )
        : QStringLiteral( "-" );

    const QString p = QStringLiteral( "▶ " )
        + formatPosition( position, map, sampleRate, barsMode );
    if( pos_->text() != p ) pos_->setText( p );

    QString s;
    // A zero-length range is no selection: the arranger itself clears one
    // that a click without a drag would make.
    if( hasRange && rangeEnd > rangeStart ) {
        s = QStringLiteral( "[ %1 – %2 ] (%3)" )
            .arg( formatPosition( rangeStart, map, sampleRate, barsMode ),
                  formatPosition( rangeEnd, map, sampleRate, barsMode ),
                  formatLength( rangeEnd - rangeStart, map, sampleRate,
                                barsMode ) );
    } else {
        s = QStringLiteral( "[ — ]" );
    }
    if( sel_->text() != s ) sel_->setText( s );
}

QString STransportReadout::describe() const
{
    return QStringLiteral( "pos=%1|sel=%2|mode=%3|selFrames=%4" )
        .arg( pos_->text(), sel_->text(),
              bars_ ? QStringLiteral( "bars" ) : QStringLiteral( "time" ),
              selFrames_ );
}
