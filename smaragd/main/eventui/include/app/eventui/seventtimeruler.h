#ifndef _SEVENTTIMERULER_H_
#define _SEVENTTIMERULER_H_

#include <QString>
#include <QWidget>

class SEventTimeAxis;

/**
 * SEventTimeRuler - the event editor's bars.beats.ticks strip (proposal 37
 * 6.2).
 *
 * It reads the PROJECT'S TEMPO MAP, not a BPM scalar: the map is the single
 * tempo authority (D2), it stores microseconds per quarter the way SMF does,
 * and the arranger ruler's 480-PPQ display becomes the map's 960 here. Reading
 * `SProject::getBPMTempo()` and multiplying would reintroduce exactly the
 * tenth-of-a-microsecond disagreement the map exists to remove.
 *
 * `leftInset` is the piano roll's keyboard column: the ruler must start where
 * the note grid starts or every bar line is 40 px off the notes underneath it.
 */
class SEventTimeRuler : public QWidget
{
    Q_OBJECT
public:
    explicit SEventTimeRuler( QWidget *parent = nullptr );

    void setTimeAxis( SEventTimeAxis *axis );
    void setLeftInset( int px );

    /** The grid division shown as subdivisions ("1/16", "1/8t"). */
    void setGrid( const QString &grid );

    QSize sizeHint() const override;

protected:
    void paintEvent( QPaintEvent * ) override;

private:
    SEventTimeAxis *axis_ = nullptr;
    int             leftInset_ = 0;
    QString         grid_ = QStringLiteral( "1/16" );
};

#endif // _SEVENTTIMERULER_H_
