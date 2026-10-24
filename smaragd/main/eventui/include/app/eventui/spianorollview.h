#ifndef _SPIANOROLLVIEW_H_
#define _SPIANOROLLVIEW_H_

#include <QList>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <vector>

#include "app/eventui/seventeditorview.h"

class QScrollBar;
class QTimer;
class QWheelEvent;

/**
 * SPianoRollView - the first registered event editor kind ("pianoroll",
 * proposal 37 6.2).
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

    /** The note grid's time span: right of the keys, left of the key bar. */
    int timeLeft() const override { return KEYS_W; }
    int timeRight() const override;

    /** True while a drag past an edge is scrolling the view (QBX-216). */
    bool autoScrolling() const;
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
     * `mods` are delivered ON THE EVENT, exactly as drag-clip-edge's do — so
     * Ctrl-drag-copies-the-body (AC-a2) is drivable from a script the same
     * way Ctrl-stretch is on a clip edge. Applied to every press/move/release
     * in the gesture, matching a real held-modifier drag.
     *
     * Returns false when the note is not there or the geometry is degenerate.
     */
    bool tkDragNote( qint64 tick, int key, int channel, qint64 toTick,
                     int toKey, const QString &edge, const QString &lane,
                     double toValue,
                     Qt::KeyboardModifiers mods = Qt::NoModifier );

    /**
     * TEST ENTRY POINT for the vertical key scrollbar - drives the REAL
     * QScrollBar's setValue() (not a re-spelling of the clamp), so the case
     * exercises the same code a drag of the scrollbar handle would. `topKey`
     * is the key requested at the TOP of the grid; it is clamped into the
     * currently reachable range exactly as a scrollbar drag would clamp at
     * its ends. Returns the resulting topKey() so a case can assert either
     * end of the MIDI range (0-127) is actually reachable.
     */
    int tkScrollKeys( int topKey );

    /**
     * TEST ENTRY POINT for the KEYBOARD, which nothing else could reach
     * (QBX-203). It sends REAL QKeyEvents through this widget's own event
     * handling, so a case exercises `keyPressEvent` itself -- the nudge, the
     * delete, the select-all, and the auto-repeat bookkeeping that decides
     * how many UNDO STEPS a hold costs.
     *
     * `repeats` is how many AUTO-REPEAT presses follow the first one, with
     * `autorep` set on those events exactly as the window system sets it on a
     * held key. That is the whole reason this seam exists: one hold must be
     * one undo step and two separate presses must be two, and no mouse verb
     * can express the difference.
     *
     * `keyName` is "left", "right", "up", "down", "delete", "backspace" or
     * "select-all". Returns false for anything else.
     *
     * Every press is offered to the SHORTCUT MAP first, as Qt does with a real
     * one (QBX-221): a ShortcutOverride nobody accepts lets a matching window
     * QAction fire INSTEAD, and the press is then not delivered at all. That
     * is the path on which the arranger's "Remove sample" used to take Delete
     * away from this view.
     */
    bool tkPressKey( const QString &keyName, int repeats = 0 );

    /**
     * TEST ENTRY POINT for a DOUBLE-click on the note grid at (tick, key) in
     * CONTENT ticks (QBX-221): the real press / release / double-click /
     * release sequence Qt delivers, so mouseDoubleClickEvent itself is under
     * test. Returns false when no clip is bound or the spot is off the grid.
     */
    bool tkDoubleClick( qint64 tick, int key );

    /**
     * TEST ENTRY POINT for AUTO-SCROLL (QBX-216). Presses at (tick, key) - on
     * a note it is a move, on empty grid a marquee - moves PAST `edge`
     * ("right", "left", "top" or "bottom") with real events, runs `steps`
     * auto-scroll steps (what the timer would run, without waiting for it),
     * and releases where the pointer is. The press must be on screen.
     *
     * Returns false unless the real move STARTED the auto-scroll timer - so a
     * case cannot pass on the steps alone when the gesture never arms it -
     * or when the press is off screen or nothing is bound.
     */
    bool tkDragPastEdge( qint64 tick, int key, const QString &edge, int steps );

protected:
    void paintEvent( QPaintEvent * ) override;
    void mousePressEvent( QMouseEvent * ) override;
    void mouseMoveEvent( QMouseEvent * ) override;
    void mouseReleaseEvent( QMouseEvent * ) override;
    void mouseDoubleClickEvent( QMouseEvent * ) override;
    void keyPressEvent( QKeyEvent * ) override;
    void resizeEvent( QResizeEvent * ) override;
    void wheelEvent( QWheelEvent * ) override;
    // AC-a3: claim Ctrl/Cmd-A for THIS view before it can become an ambiguous
    // WindowShortcut. Qt asks a focused widget's event() for
    // QEvent::ShortcutOverride before dispatching a QAction shortcut with
    // Window/ApplicationShortcut context; accepting it here is what routes
    // Ctrl-A into keyPressEvent() below (select every note) instead of into
    // the shell's "Select All" QAction (select every clip) — the same trick
    // QLineEdit uses internally to keep its own Ctrl-A "select all text".
    bool event( QEvent * ) override;

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

    // Position/resize keyScroll_ over the note-grid band and sync its range
    // and value to the current geometry (gridHeight()/keyHeight_) and
    // topKey_. Called whenever either could have changed: resizeEvent(),
    // addCcLane()/removeCcLane() (they change gridHeight() without a
    // resize), and refresh() (it can re-centre topKey_).
    void layoutKeyScroll();

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
    // Copy (AC-a2): Ctrl-drag on a note BODY. A distinct kind from Move
    // because previewNotes() has to APPEND a new SEvent rather than mutate
    // the dragged one in place — the whole point is that the original stays
    // exactly where it was.
    enum class DragKind { None, Move, Resize, Marquee, Velocity, Cc, Copy };
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

    /** Apply `drag_` to the note table and return the new absolute state.
     *
     * `touchedIds`, when non-null, is filled with the POST-drag idStr() of
     * every note the gesture actually touched (Move/Resize/Velocity mutate in
     * place, so this is the id AFTER the mutation — the address the note now
     * lives at). commitDrag() uses this instead of re-deriving "which notes
     * moved" from `selected_`, which is exactly the bug this parameter fixes:
     * checking `selected_.contains(idStr(e))` against the POST-drag table
     * answers "is this note's new address in the OLD selection", which for a
     * Resize (the id does not depend on duration) is true for every note in
     * the whole selection whenever `wholeSelection` is true — i.e. almost
     * always, since mousePressEvent already selects the grabbed note before a
     * drag starts. Left unfilled for Copy, which computes its own touched set
     * by diffing against the pre-drag table (a copy never reuses an existing
     * id, so that diff is exact and self-contained). */
    std::vector<SEvent> previewNotes( const Resolved &r,
                                      const std::vector<SEvent> &notes,
                                      QSet<QString> *touchedIds = nullptr ) const;

    void commitDrag();

    // --- auto-scroll (QBX-216) ---------------------------------------------
    bool isAutoScrollDrag() const;
    /** Start the timer while a positional drag is past an edge, else stop. */
    void updateAutoScroll();
    /** One step: pan the axis / scroll the keys toward the pointer and move
     *  the drag's press anchor with the content. True if anything moved. */
    bool autoScrollStep();
    QTimer *autoScroll_ = nullptr;
    void selectOnly( const QString &id );
    void nudgeSelection( qint64 dTicks, int dKey );

    // THE CURRENT NUDGE GESTURE (QBX-203). Refreshed on an arrow press that is
    // not an auto-repeat, so one HOLD is one undo step while two separate
    // presses are two. Every other edit in this view commits once and passes
    // nothing.
    quint64 nudgeGesture_ = 0;

    QSet<QString> selected_;
    QList<int>    ccLanes_;
    int           topKey_    = 84;
    // FIXED for now (CONTRACT "known debt": no zoom of its own on the
    // vertical axis yet). Should become a per-view zoom setting later,
    // exactly as the horizontal axis already is via SEventTimeAxis; until
    // then this one constant is the only place a taller/shorter key row
    // would be changed.
    int           keyHeight_ = 6;
    QScrollBar   *keyScroll_ = nullptr;   // the note grid's vertical scroll
    int           noteCount_ = 0;
    /** The length a double-click insert gets, in ticks; 0 = one grid division. */
    qint64        drawDurTicks_ = 0;
    double        drawVelocity_ = 100.0;

    // --- mouse-wheel pan/zoom (matches the arranger's default gesture
    // mapping; own SOpt::Event* keys, see main/eventui/CONTRACT.md and
    // main/servicesui's Event Editor options page) ------------------------
    //
    // Plain wheel scrolls the key rows (keyScroll_); Shift+wheel pans the
    // shared SEventTimeAxis; Ctrl and Ctrl+Shift zoom it (horizontal) or the
    // key-row height (vertical) -- SOpt::WheelAction is reused verbatim, only
    // the SOpt KEYS and hence the stored mapping are the piano roll's own.
    void loadWheelConfig();
    int  wheelActionFor( Qt::KeyboardModifiers mods ) const;
    bool applyWheel( QWheelEvent *ev );

    int    wheelPlain_ = 0, wheelShift_ = 0, wheelCtrl_ = 0, wheelCtrlShift_ = 0;
    bool   wheelZoomToCursor_ = true, wheelInvertZoom_ = false;
    // SOpt::EventWheelSensitivityPct / 100, clamped, plus the derived per-
    // gesture constants below -- cached rather than recomputed per event
    // because a wheel event is a hot path (mirrors SMVActualView's own
    // wheelSensitivity_ cache).
    double wheelSensitivity_ = 1.0;
    int    wheelVScrollStep_ = 600;    // angleDelta units per key row
    double wheelZoomHFactor_ = 1.2;    // px/second multiplier per notch
    double wheelZoomVFactor_ = 1.5;    // key-row-height multiplier per notch
    int    wheelVScrollAccum_ = 0;     // accumulated sub-notch vertical delta
};

#endif // _SPIANOROLLVIEW_H_
