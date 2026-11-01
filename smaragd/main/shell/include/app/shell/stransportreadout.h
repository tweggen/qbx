#ifndef _STRANSPORTREADOUT_H_
#define _STRANSPORTREADOUT_H_

#include <QString>
#include <QWidget>

#include "tw/graph/tw303aenv.h"   // offset_t

class QLabel;
class twTempoMap;

/**
 * STransportReadout - the transport toolbar's POSITION and TIME SELECTION
 * readout (QBX-215): where the playhead is, and the range the arranger's ruler
 * shows as start - end (length).
 *
 * A dumb display. SMainWindow gathers the state - the locator, the ACTIVE
 * arranger's range (which may be a default the arranger made up rather than a
 * stored property, so it is read from the view and not from SProjectProps),
 * the project's tempo map and the ruler mode - and hands it to setState(). The
 * widget only formats, and only touches a label whose text changed: the
 * playhead moves it thirty times a second.
 *
 * THE WIDTH DOES NOT FOLLOW THE POSITION. The digits are fixed-width AND the
 * position label reserves the width of the longest reading it can show, because
 * the first alone does not stop the LABEL from resizing when the bar number
 * gains a digit -- which slid the tempo box, the master meter, the latency
 * readout and every toolbar sharing the row. See the constructor.
 *
 * Positions are formatted the way the ruler shows them: "bars" mode as
 * bar.beat.tick (1-based bar and beat, ticks within the beat), "time" mode as
 * m:ss.mmm. A length is the same in 0-based form ("4.0.000" is four bars).
 * Bars go through the project's twTempoMap and nothing else - frames to ticks
 * EXACTLY, then the NEAREST tick, because a range snapped to a bar line sits
 * on a whole frame while the bar line itself, at most tempi, does not: a
 * floor would read bar 5 as 4.4.959.
 */
class STransportReadout : public QWidget
{
    Q_OBJECT
public:
    explicit STransportReadout( QWidget *parent = nullptr );

    void setState( offset_t position, bool hasRange, offset_t rangeStart,
                   offset_t rangeEnd, const twTempoMap &map, int sampleRate,
                   bool barsMode );

    /** The test face: `pos=<shown>|sel=<shown>|mode=<bars|time>`, built
     *  from the labels' own text, so what is asserted is what is shown;
     *  then `|selFrames=<start>-<end>` (or `-`), the range's raw frames as
     *  last shown, which no label can carry and an exactness claim needs
     *  (QBX-212); then `|posPinned=<0|1>`, true while the position's own hint
     *  fits the reserved width -- i.e. while the playhead cannot move anything
     *  right of the readout. Fields are APPENDED, never inserted. */
    QString describe() const;

    // The formatting, exposed for setState() and for anyone else that must
    // spell a position the way the ruler does.
    static QString formatPosition( offset_t frame, const twTempoMap &map,
                                   int sampleRate, bool barsMode );
    static QString formatLength( offset_t frames, const twTempoMap &map,
                                 int sampleRate, bool barsMode );

private:
    QLabel *pos_  = nullptr;
    QLabel *sel_  = nullptr;
    bool    bars_ = true;
    QString selFrames_ = QStringLiteral( "-" );
};

#endif // _STRANSPORTREADOUT_H_
