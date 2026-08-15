#ifndef _SPIANOROLLVIEW_H_
#define _SPIANOROLLVIEW_H_

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <vector>

#include "app/eventui/seventeditorview.h"

/**
 * SPianoRollView - the first registered event editor kind ("pianoroll",
 * proposal 36 6.2).
 *
 * ONE widget, three horizontal BANDS, one set of mouse handlers that dispatch
 * on which band the press landed in:
 *
 *     [ keys | note grid          ]   notes: draw / erase / select / move /
 *     [ VEL  | velocity lane      ]          resize / marquee
 *     [ CC n | CC lane            ]   one lane per shown controller
 *
 * Bands rather than child widgets on purpose: every band shares the ONE time
 * axis and must stay column-aligned with the ruler and with the arranger above
 * it, and three sibling widgets with three layouts is precisely how that
 * alignment drifts. It also means `drag-note lane="velocity"` drives the same
 * real handlers a user's pointer does, which is the whole point of the verb.
 *
 * The vertical axis is keys: `topKey_` is the note drawn at the top of the
 * grid and `keyHeight_` its pixel height, so `y = (topKey_ - key)*keyHeight_`.
 * setClip() centres the range on the clip's own notes, which is what makes a
 * headless assertion (and a freshly opened dock) show something.
 *
 * A LIVE drag never touches the model: it paints out of `drag_` and the
 * release REVERTS that preview and submits ONE action (base-class rule 2,
 * timeline invariant 3). There is therefore nothing to roll back if the
 * gesture is cancelled - the model was never wrong.
 */
class SPianoRollView : public SEventEditorView
{
    Q_OBJECT
public:
    explicit SPianoRollView( QWidget *parent = nullptr );
    ~SPianoRollView() override;

    QString kind() const override { return QStringLiteral( "pianoroll" ); }

    void refresh() override;
    QString describeExtra() const override;
    int noteCount() const override { return noteCount_; }
    int selectionCount() const override { return selected_.size(); }

    /** Controller lanes shown under the velocity lane (CC numbers). */
    void addCcLane( int controller );
    void removeCcLane( int controller );
    const QList<int> &ccLanes() const { return ccLanes_; }

    /**
     * TEST ENTRY POINT for `drag-note` - the `drag-clip-edge` twin. Addresses
     * the note by (tick, key, channel) in CONTENT ticks, works out where that
     * note is on screen and sends REAL press/move/release events to itself, so
     * the handlers under test are the ones a pointer drives. Never synthesizes
     * OS input.
     *
     *   lane="notes"    : move to (toTick, toKey); edge="end" resizes instead
     *   lane="velocity" : drag the velocity lane bar to toValue (0..127)
     *
     * Returns false when the note is not there or the geometry is degenerate.
     */
    bool tkDragNote( qint64 tick, int key, int channel, qint64 toTick,
                     int toKey, const QString &edge, const QString &lane,
                     double toValue );

protected:
    void paintEvent( QPaintEvent * ) override;
    void mousePressEvent( QMouseEvent * ) override;
    void mouseMoveEvent( QMouseEvent * ) override;
    void mouseReleaseEvent( QMouseEvent * ) override;
    void keyPressEvent( QKeyEvent * ) override;
    void resizeEvent( QResizeEvent * ) override;

private:
    // --- band geometry ----------------------------------------------------
    static constexpr int KEYS_W  = 40;   // the painted keyboard / lane labels
    static constexpr int VEL_H   = 48;
    static constexpr int CC_H    = 40;
    static constexpr int EDGE_PX = 4;    // the resize grab band on a note's end

    int gridHeight() const;
    int velTop() const { return gridHeight(); }
    int ccTop( int laneIdx ) const { return velTop() + VEL_H + laneIdx * CC_H; }
    enum class Band { None, Grid, Velocity, Cc };
    Band bandAt( int y, int *ccLaneOut = nullptr ) const;

    int    yOfKey( int key ) const;
    int    keyOfY( int y ) const;

    // A note's address, which is what survives a re-resolve (never a pointer).
    struct NoteId {
        qint64 tick = 0; int key = -1; int channel = -1;
        QString str() const;
    };
    static QString idStr( const SEvent &e );

    QRect  noteRect( const Resolved &r, const SEvent &e ) const;
    /** Index into `notes` of the note under `pos`, or -1. */
    int    noteAt( const Resolved &r, const std::vector<SEvent> &notes,
                   const QPoint &pos, bool *onEndEdge = nullptr ) const;

    // --- the live gesture -------------------------------------------------
    enum class DragKind { None, Move, Resize, Marquee, Velocity, Cc };
    struct Drag {
        DragKind kind = DragKind::None;
        QPoint   press;
        QPoint   cur;
        qint64   pressTick = 0;
        int      pressKey  = -1;
        QString  anchor;       // idStr of the note the gesture grabbed
        double   value = 0.0;  // velocity / CC lane target
        int      ccLane = -1;  // which CC lane a Cc gesture is drawing in
    };
    Drag drag_;

    /** Apply `drag_` to the note table and return the new absolute state. */
    std::vector<SEvent> previewNotes( const Resolved &r,
                                      const std::vector<SEvent> &notes ) const;

    void commitDrag();
    void selectOnly( const QString &id );
    void nudgeSelection( qint64 dTicks, int dKey );

    QSet<QString> selected_;
    QList<int>    ccLanes_;
    int           topKey_    = 84;
    int           keyHeight_ = 8;
    int           noteCount_ = 0;
    /** The length a Draw-tool insert gets, in ticks; 0 = one grid division. */
    qint64        drawDurTicks_ = 0;
    double        drawVelocity_ = 100.0;
};

#endif // _SPIANOROLLVIEW_H_
