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

    // Fixed-width digits, so the GLYPHS do not shuffle while the playhead
    // runs. Measured on the dev box: DejaVu Sans Mono, every digit 7.22 px,
    // spread 0.00 -- and the application font (the bundled FreeSans) has
    // tabular digits too, so this was never the twitch anyone saw.
    QFont f = font();
    f.setFamily( QStringLiteral( "monospace" ) );
    f.setStyleHint( QFont::TypeWriter );
    pos_->setFont( f );
    sel_->setFont( f );

    // ...AND THE LABEL'S WIDTH IS RESERVED, WHICH IS THE OTHER HALF.
    //
    // Equal-width digits keep the glyphs still; they do not keep the LABEL
    // still. "9.4.959" is nine characters and "10.1.000" is ten, so the hint
    // grew by one character the moment the playhead crossed bar 10 and
    // everything to the right of the readout moved with it: the tempo box, the
    // master meter, the latency readout, and -- because the toolbar's own
    // width changed -- every toolbar sharing the row. Measured in the running
    // app through describe(): hint 65 px up to bar 9, 73 px from bar 10, and
    // another 7 px at each further digit.
    //
    // So the label asks for the WIDEST position it will ever show and keeps
    // that width whatever it is displaying. Four bar digits covers bar 9999,
    // and is also the longest "time" reading ("1000:00.000" is one character
    // longer and arrives after sixteen hours). A minimum rather than
    // setFixedWidth: past the reservation the label grows once more, which is
    // correct-and-still-legible where a fixed width would elide the bar
    // number.
    // THE RESERVATION IS THE LABEL'S OWN HINT FOR THE WIDEST READING, not the
    // text's advance: a QLabel's hint carries its margins, so reserving the
    // advance alone leaves the hint a few px above the reservation for every
    // text of the full length -- and `posPinned` would then be answered by the
    // margin rather than by the position. Setting the widest text and reading
    // the hint back asks the widget the same question the layout will.
    pos_->setText( QStringLiteral( "\u25B6 0000.0.000" ) );
    pos_->setMinimumWidth( pos_->sizeHint().width() );
    // NOT the selection label, deliberately. It changes only when the user
    // changes the selection -- never with the playhead -- so reserving its
    // worst case ("[ 0000.0.000 - 0000.0.000 ] (0000.0.000)", some 300 px)
    // would spend the toolbar's scarcest resource to hold still something that
    // only moves when the user moves it.

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
    // APPENDED, never inserted: the assert-* verbs match substrings of this
    // line, so a field put between two existing ones breaks every assertion
    // spanning the seam.
    //
    // `posPinned` is the claim the gate needs and the one no pixel can make
    // headlessly (a test run never show()s a window, so no widget is ever
    // mapped and its rectangle means nothing): the position label's own hint
    // FITS inside the width it reserved, so the width the layout gives it is
    // that reserved CONSTANT and no position can move anything right of it. A
    // px figure would be a font-and-DPI number and could not be asserted on
    // another box; this is a yes/no about the invariant itself.
    //
    // IT READS THE WIDGET'S OWN minimumWidth(), not a remembered reservation:
    // that is what the layout reads, so a build that computes the reservation
    // and forgets to apply it cannot pass -- the SSoloIndicator::describe()
    // discipline ("the WIDGET's own state, not our intent"). Measured on two
    // sabotages: with the setMinimumWidth() call removed the field reads 0 at
    // EVERY position (all five assertions of the case fail), and with the
    // reservation cut to ONE bar digit it reads 0 from bar 10 on (three of
    // them fail and the two at bar 1 pass) -- which is what makes the case's
    // four digit counts each earn their place.
    return QStringLiteral( "pos=%1|sel=%2|mode=%3|selFrames=%4" )
        .arg( pos_->text(), sel_->text(),
              bars_ ? QStringLiteral( "bars" ) : QStringLiteral( "time" ),
              selFrames_ )
        + QStringLiteral( "|posPinned=%1" )
              .arg( pos_->minimumWidth() > 0
                        && pos_->sizeHint().width() <= pos_->minimumWidth()
                    ? 1 : 0 );
}
