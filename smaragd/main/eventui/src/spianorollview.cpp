#include "app/eventui/spianorollview.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>

#include "app/eventui/seventtimeaxis.h"

#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidieventactions.h"
#include "app/shell/sapplication.h"

namespace {

// A black key in every octave, so the grid reads as a keyboard rather than as
// squared paper.
bool isBlackKey( int key )
{
    switch( ( ( key % 12 ) + 12 ) % 12 ) {
    case 1: case 3: case 6: case 8: case 10: return true;
    default: return false;
    }
}

double clampVel( double v )
{
    return v < 1.0 ? 1.0 : ( v > 127.0 ? 127.0 : v );
}

bool sameNotes( const std::vector<SEvent> &a, const std::vector<SEvent> &b )
{
    if( a.size() != b.size() ) return false;
    for( size_t i = 0; i < a.size(); ++i ) {
        if( a[ i ].t != b[ i ].t || a[ i ].dur != b[ i ].dur
            || a[ i ].key != b[ i ].key || a[ i ].channel != b[ i ].channel
            || a[ i ].value != b[ i ].value ) return false;
    }
    return true;
}

}  // namespace

SPianoRollView::SPianoRollView( QWidget *parent )
    : SEventEditorView( parent )
{
    setFocusPolicy( Qt::StrongFocus );
    setMinimumHeight( 120 );

    // The note grid's vertical scroll: the full 0-127 MIDI pitch range is
    // rarely visible at once at a 6 px row height, so it has to be reachable
    // by scrolling rather than only by refresh()'s auto-centre-on-the-notes
    // (that still runs first and picks a sensible default, same as before).
    keyScroll_ = new QScrollBar( Qt::Vertical, this );
    connect( keyScroll_, &QScrollBar::valueChanged, this, [this]( int v ) {
        // Value 0 (the scrollbar's own top) shows the HIGHEST keys, so this
        // is an inversion rather than a direct assignment - see
        // layoutKeyScroll() for the matching forward mapping.
        topKey_ = 127 - v;
        update();
    } );
    layoutKeyScroll();
}

SPianoRollView::~SPianoRollView() = default;

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

int SPianoRollView::gridHeight() const
{
    const int h = height() - VEL_H - ccLanes_.size() * CC_H;
    return h < 40 ? 40 : h;
}

SPianoRollView::Band SPianoRollView::bandAt( int y, int *ccLaneOut ) const
{
    if( ccLaneOut ) *ccLaneOut = -1;
    if( y < 0 ) return Band::None;
    if( y < velTop() ) return Band::Grid;
    if( y < velTop() + VEL_H ) return Band::Velocity;
    const int rel = y - ( velTop() + VEL_H );
    const int lane = rel / CC_H;
    if( lane >= 0 && lane < ccLanes_.size() ) {
        if( ccLaneOut ) *ccLaneOut = lane;
        return Band::Cc;
    }
    return Band::None;
}

int SPianoRollView::yOfKey( int key ) const
{
    return ( topKey_ - key ) * keyHeight_;
}

int SPianoRollView::keyOfY( int y ) const
{
    // Floor division: a y inside a row belongs to that row; above the top of
    // the grid belongs to a higher key.
    const int row = ( y >= 0 ) ? ( y / keyHeight_ )
                               : ( -( ( -y + keyHeight_ - 1 ) / keyHeight_ ) );
    int k = topKey_ - row;
    if( k < 0 ) k = 0;
    if( k > 127 ) k = 127;
    return k;
}

void SPianoRollView::layoutKeyScroll()
{
    if( !keyScroll_ ) return;

    const int gh = gridHeight();
    const int scrollW = keyScroll_->sizeHint().width();
    keyScroll_->setGeometry( width() - scrollW, 0, scrollW, gh );

    const int rows = std::max( 1, gh / keyHeight_ );
    // Value 0 = topKey_ 127 (the highest keys, at the scrollbar's own top);
    // value maxV = topKey_ rows-1 (key 0 sits in the bottom row) - the whole
    // 0..127 range is reachable by construction, whatever gridHeight() is.
    const int maxV = std::max( 0, 128 - rows );
    const QSignalBlocker block( keyScroll_ );   // this is a SYNC, not a gesture
    keyScroll_->setRange( 0, maxV );
    keyScroll_->setPageStep( rows );
    keyScroll_->setValue( std::max( 0, std::min( maxV, 127 - topKey_ ) ) );
}

QString SPianoRollView::NoteId::str() const
{
    return QStringLiteral( "%1:%2:%3" ).arg( tick ).arg( key ).arg( channel );
}

QString SPianoRollView::idStr( const SEvent &e )
{
    return QStringLiteral( "%1:%2:%3" ).arg( e.t ).arg( e.key ).arg( e.channel );
}

QRect SPianoRollView::noteRect( const Resolved &r, const SEvent &e ) const
{
    const int x0 = xOfTick( r, e.t ) + KEYS_W;
    const int x1 = xOfTick( r, e.t + ( e.dur > 0 ? e.dur : 1 ) ) + KEYS_W;
    const int y  = yOfKey( e.key );
    int w = x1 - x0;
    if( w < 2 ) w = 2;
    return QRect( x0, y, w, keyHeight_ );
}

int SPianoRollView::noteAt( const Resolved &r, const std::vector<SEvent> &notes,
                            const QPoint &pos, bool *onEndEdge ) const
{
    if( onEndEdge ) *onEndEdge = false;
    for( int i = (int) notes.size() - 1; i >= 0; --i ) {
        const QRect nr = noteRect( r, notes[ (size_t) i ] );
        if( !nr.contains( pos ) ) continue;
        if( onEndEdge ) *onEndEdge = ( pos.x() >= nr.right() - EDGE_PX );
        return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void SPianoRollView::refresh()
{
    const Resolved r = resolve();
    const std::vector<SEvent> notes = notesOf( r );
    noteCount_ = (int) notes.size();

    // Prune a selection whose notes are gone (an undo, another editor, a
    // reload). Ids are ADDRESSES, not pointers, so this is the whole cleanup.
    QSet<QString> live;
    for( const SEvent &e : notes ) live.insert( idStr( e ) );
    selected_.intersect( live );

    // Centre the key range on what the clip actually holds, so a freshly bound
    // view - and a headless assertion, which never scrolls - shows the notes.
    if( !notes.empty() ) {
        int lo = 127, hi = 0;
        for( const SEvent &e : notes ) {
            lo = std::min( lo, e.key );
            hi = std::max( hi, e.key );
        }
        const int rows = std::max( 1, gridHeight() / keyHeight_ );
        if( hi > topKey_ || lo <= topKey_ - rows ) {
            int want = hi + 2;
            if( want > 127 ) want = 127;
            if( want < rows - 1 ) want = rows - 1;
            topKey_ = want;
        }
    }
    layoutKeyScroll();
    update();
}

QString SPianoRollView::describeExtra() const
{
    const char *tool = tool_ == Tool::Draw ? "draw"
                     : tool_ == Tool::Erase ? "erase" : "select";
    // botKey is the LOWEST key currently visible in the grid - together with
    // topKey_ it is what a case checks the full 0-127 range against after
    // scrolling to either end (tkScrollKeys).
    const int rows = std::max( 1, gridHeight() / keyHeight_ );
    const int botKey = std::max( 0, topKey_ - ( rows - 1 ) );
    return QStringLiteral( "tool=%1|topKey=%2|keyH=%3|ccLanes=%4|botKey=%5" )
        .arg( QLatin1String( tool ) ).arg( topKey_ ).arg( keyHeight_ )
        .arg( ccLanes_.size() ).arg( botKey );
}

void SPianoRollView::addCcLane( int controller )
{
    if( controller < 0 || controller > 127 ) return;
    if( ccLanes_.contains( controller ) ) return;
    ccLanes_.append( controller );
    layoutKeyScroll();
    update();
}

void SPianoRollView::removeCcLane( int controller )
{
    ccLanes_.removeAll( controller );
    layoutKeyScroll();
    update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void SPianoRollView::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    const QRect all = rect();
    p.fillRect( all, QColor( 40, 42, 46 ) );

    const Resolved r = resolve();
    const int gh = gridHeight();

    // --- key rows + the painted keyboard ---------------------------------
    const int rows = gh / keyHeight_ + 1;
    for( int i = 0; i < rows; ++i ) {
        const int key = topKey_ - i;
        if( key < 0 ) break;
        const int y = i * keyHeight_;
        const QRect row( KEYS_W, y, all.width() - KEYS_W, keyHeight_ );
        p.fillRect( row, isBlackKey( key ) ? QColor( 34, 36, 40 )
                                           : QColor( 48, 50, 55 ) );
        p.fillRect( QRect( 0, y, KEYS_W, keyHeight_ ),
                    isBlackKey( key ) ? QColor( 25, 25, 28 )
                                      : QColor( 225, 225, 225 ) );
        p.setPen( QColor( 30, 31, 34 ) );
        p.drawLine( 0, y, all.width(), y );
        if( key % 12 == 0 && keyHeight_ >= 8 ) {
            p.setPen( QColor( 90, 90, 90 ) );
            p.drawText( QRect( 2, y, KEYS_W - 4, keyHeight_ ),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        QStringLiteral( "C%1" ).arg( key / 12 - 1 ) );
        }
    }

    // --- the grid, straight off the tempo map through the shared axis -----
    if( r.valid() && axis_ ) {
        const qint64 g = gridTicks( r );
        if( g > 0 ) {
            const qint64 firstTick = tickOfX( r, 0 );
            qint64 t = ( firstTick / g ) * g;
            if( t > firstTick ) t -= g;
            const qint64 beat = r.seq ? r.seq->ppq() : 960;
            for( int guard = 0; guard < 4096; ++guard, t += g ) {
                const int x = xOfTick( r, t ) + KEYS_W;
                if( x > all.width() ) break;
                if( x < KEYS_W ) continue;
                const bool onBeat = ( beat > 0 && ( t % beat ) == 0 );
                p.setPen( onBeat ? QColor( 84, 86, 92 ) : QColor( 58, 60, 65 ) );
                p.drawLine( x, 0, x, gh );
            }
        }
    }

    // --- notes (the DRAG PREVIEW, when one is live) -----------------------
    std::vector<SEvent> notes = notesOf( r );
    if( drag_.kind == DragKind::Move || drag_.kind == DragKind::Resize
        || drag_.kind == DragKind::Velocity )
        notes = previewNotes( r, notes );

    for( const SEvent &e : notes ) {
        QRect nr = noteRect( r, e );
        if( nr.bottom() < 0 || nr.top() > gh ) continue;
        nr = nr.intersected( QRect( KEYS_W, 0, all.width() - KEYS_W, gh ) );
        if( nr.isEmpty() ) continue;
        const bool sel = selected_.contains( idStr( e ) );
        const int shade = (int) ( 90.0 + 120.0 * clampVel( e.value ) / 127.0 );
        p.fillRect( nr, sel ? QColor( 250, 210, 90 )
                            : QColor( 70, shade, 220 ) );
        p.setPen( QColor( 20, 20, 25 ) );
        p.drawRect( nr.adjusted( 0, 0, -1, -1 ) );
    }

    // --- velocity lane ----------------------------------------------------
    const int vt = velTop();
    p.fillRect( QRect( 0, vt, all.width(), VEL_H ), QColor( 30, 32, 36 ) );
    p.setPen( QColor( 70, 72, 78 ) );
    p.drawLine( 0, vt, all.width(), vt );
    p.setPen( QColor( 150, 150, 150 ) );
    p.drawText( QRect( 2, vt, KEYS_W - 4, VEL_H ),
                Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral( "VEL" ) );
    for( const SEvent &e : notes ) {
        const int x = xOfTick( r, e.t ) + KEYS_W;
        if( x < KEYS_W || x > all.width() ) continue;
        const int h = (int) ( VEL_H * clampVel( e.value ) / 127.0 );
        const bool sel = selected_.contains( idStr( e ) );
        p.fillRect( QRect( x, vt + VEL_H - h, 3, h ),
                    sel ? QColor( 250, 210, 90 ) : QColor( 110, 170, 230 ) );
    }

    // --- CC lanes ---------------------------------------------------------
    std::vector<SEvent> events;
    if( r.seq ) events = r.seq->events();
    for( int lane = 0; lane < ccLanes_.size(); ++lane ) {
        const int top = ccTop( lane );
        const int cc = ccLanes_.at( lane );
        p.fillRect( QRect( 0, top, all.width(), CC_H ), QColor( 28, 30, 34 ) );
        p.setPen( QColor( 70, 72, 78 ) );
        p.drawLine( 0, top, all.width(), top );
        p.setPen( QColor( 150, 150, 150 ) );
        p.drawText( QRect( 2, top, KEYS_W - 4, CC_H ),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    QStringLiteral( "CC%1" ).arg( cc ) );
        for( const SEvent &e : events ) {
            if( e.kind != twEventKind::ControlChange ) continue;
            if( (int) e.paramId != cc ) continue;
            const int x = xOfTick( r, e.t ) + KEYS_W;
            if( x < KEYS_W || x > all.width() ) continue;
            const int h = (int) ( CC_H * clampVel( e.value ) / 127.0 );
            p.fillRect( QRect( x, top + CC_H - h, 3, h ),
                        QColor( 120, 200, 140 ) );
        }
    }

    // --- marquee ----------------------------------------------------------
    if( drag_.kind == DragKind::Marquee ) {
        const QRect m = QRect( drag_.press, drag_.cur ).normalized();
        p.setPen( QColor( 250, 210, 90 ) );
        p.drawRect( m );
    }
}

// ---------------------------------------------------------------------------
// Gestures - every one of them commits ONE action, on release
// ---------------------------------------------------------------------------

void SPianoRollView::mousePressEvent( QMouseEvent *ev )
{
    setFocus( Qt::MouseFocusReason );
    const QPoint pos = ev->position().toPoint();
    drag_ = Drag();
    drag_.press = pos;
    drag_.cur   = pos;

    const Resolved r = resolve();
    if( !r.valid() || !axis_ ) { ev->accept(); return; }
    if( pos.x() < KEYS_W ) { ev->accept(); return; }   // the keyboard column

    int ccLane = -1;
    const Band band = bandAt( pos.y(), &ccLane );
    const std::vector<SEvent> notes = notesOf( r );

    if( band == Band::Grid ) {
        bool onEdge = false;
        const int idx = noteAt( r, notes, pos, &onEdge );

        if( tool_ == Tool::Erase ) {
            if( idx >= 0 ) {
                const SEvent &e = notes[ (size_t) idx ];
                SApplication::app().submitAction(
                    new SRemoveNoteAction( clipPath_, e.t, e.key, e.channel,
                                           -1, true ) );
            }
            ev->accept();
            return;
        }
        if( tool_ == Tool::Draw ) {
            const qint64 g = gridTicks( r );
            const qint64 t = snapTick( r, tickOfX( r, pos.x() - KEYS_W ) );
            const qint64 d = drawDurTicks_ > 0 ? drawDurTicks_
                                               : ( g > 0 ? g : 240 );
            SApplication::app().submitAction(
                new SAddNoteAction( clipPath_, t, d, keyOfY( pos.y() ),
                                    drawVelocity_, 0, 64.0, -1, true ) );
            ev->accept();
            return;
        }

        // Select tool.
        if( idx >= 0 ) {
            const SEvent &e = notes[ (size_t) idx ];
            const QString id = idStr( e );
            if( ev->modifiers() & Qt::ControlModifier ) {
                if( selected_.contains( id ) ) selected_.remove( id );
                else                           selected_.insert( id );
            } else if( !selected_.contains( id ) ) {
                selectOnly( id );
            }
            drag_.kind      = onEdge ? DragKind::Resize : DragKind::Move;
            drag_.anchor    = id;
            drag_.pressTick = tickOfX( r, pos.x() - KEYS_W );
            drag_.pressKey  = keyOfY( pos.y() );
        } else {
            if( !( ev->modifiers() & Qt::ControlModifier ) ) selected_.clear();
            drag_.kind = DragKind::Marquee;
        }
        update();
        ev->accept();
        return;
    }

    if( band == Band::Velocity ) {
        // The bar under the pointer is the note whose START is nearest to x -
        // the same rule the bars are drawn by (one 3 px bar at the note start).
        int best = -1, bestDx = 1 << 30;
        for( int i = 0; i < (int) notes.size(); ++i ) {
            const int x = xOfTick( r, notes[ (size_t) i ].t ) + KEYS_W;
            const int dx = std::abs( x - pos.x() );
            if( dx < bestDx ) { bestDx = dx; best = i; }
        }
        if( best >= 0 && bestDx <= 8 ) {
            drag_.kind   = DragKind::Velocity;
            drag_.anchor = idStr( notes[ (size_t) best ] );
            drag_.value  = clampVel( 127.0 * ( velTop() + VEL_H - pos.y() )
                                     / (double) VEL_H );
            update();
        }
        ev->accept();
        return;
    }

    if( band == Band::Cc ) {
        drag_.kind   = DragKind::Cc;
        drag_.ccLane = ccLane;
        drag_.value  = clampVel( 127.0 * ( ccTop( ccLane ) + CC_H - pos.y() )
                                 / (double) CC_H );
        ev->accept();
        return;
    }
    ev->accept();
}

void SPianoRollView::mouseMoveEvent( QMouseEvent *ev )
{
    if( drag_.kind == DragKind::None ) { ev->ignore(); return; }
    drag_.cur = ev->position().toPoint();
    if( drag_.kind == DragKind::Velocity )
        drag_.value = clampVel( 127.0 * ( velTop() + VEL_H - drag_.cur.y() )
                                / (double) VEL_H );
    else if( drag_.kind == DragKind::Cc && drag_.ccLane >= 0 )
        drag_.value = clampVel( 127.0 * ( ccTop( drag_.ccLane ) + CC_H
                                          - drag_.cur.y() ) / (double) CC_H );
    update();
    ev->accept();
}

void SPianoRollView::mouseReleaseEvent( QMouseEvent *ev )
{
    drag_.cur = ev->position().toPoint();
    if( drag_.kind == DragKind::Velocity )
        drag_.value = clampVel( 127.0 * ( velTop() + VEL_H - drag_.cur.y() )
                                / (double) VEL_H );
    else if( drag_.kind == DragKind::Cc && drag_.ccLane >= 0 )
        drag_.value = clampVel( 127.0 * ( ccTop( drag_.ccLane ) + CC_H
                                          - drag_.cur.y() ) / (double) CC_H );
    commitDrag();
    ev->accept();
}

std::vector<SEvent> SPianoRollView::previewNotes(
    const Resolved &r, const std::vector<SEvent> &notes ) const
{
    std::vector<SEvent> out = notes;
    if( !r.valid() ) return out;

    // Which notes the gesture moves: the SELECTION when the grabbed note is
    // part of it, the grabbed note alone otherwise. The house rule, the same
    // one a track-level gesture aimed into a selection follows (timeline
    // invariant 12).
    const bool wholeSelection = selected_.contains( drag_.anchor );

    // The ANCHOR's new position decides the delta for everyone, so a multi-note
    // drag keeps its internal spacing exactly.
    qint64 anchorTick = 0, anchorDur = 0;
    int anchorKey = -1;
    for( const SEvent &e : notes ) {
        if( idStr( e ) == drag_.anchor ) {
            anchorTick = e.t; anchorDur = e.dur; anchorKey = e.key; break;
        }
    }
    if( anchorKey < 0 ) return out;

    if( drag_.kind == DragKind::Move ) {
        const qint64 dRaw = tickOfX( r, drag_.cur.x() - KEYS_W )
                          - tickOfX( r, drag_.press.x() - KEYS_W );
        qint64 newTick = snapTick( r, anchorTick + dRaw );
        if( newTick < 0 ) newTick = 0;
        const qint64 dTick = newTick - anchorTick;
        const int dKey = keyOfY( drag_.cur.y() ) - keyOfY( drag_.press.y() );
        for( SEvent &e : out ) {
            if( !( wholeSelection ? selected_.contains( idStr( e ) )
                                  : idStr( e ) == drag_.anchor ) ) continue;
            e.t = std::max<qint64>( 0, e.t + dTick );
            e.key = std::min( 127, std::max( 0, e.key + dKey ) );
        }
    } else if( drag_.kind == DragKind::Resize ) {
        const qint64 minDur = std::max<qint64>( 1, gridTicks( r ) / 8 );
        const qint64 newEnd = snapTick( r, tickOfX( r, drag_.cur.x() - KEYS_W ) );
        qint64 newDur = newEnd - anchorTick;
        if( newDur < minDur ) newDur = minDur;
        const qint64 dDur = newDur - anchorDur;
        for( SEvent &e : out ) {
            if( !( wholeSelection ? selected_.contains( idStr( e ) )
                                  : idStr( e ) == drag_.anchor ) ) continue;
            e.dur = std::max( minDur, e.dur + dDur );
        }
    } else if( drag_.kind == DragKind::Velocity ) {
        for( SEvent &e : out ) {
            if( !( wholeSelection ? selected_.contains( idStr( e ) )
                                  : idStr( e ) == drag_.anchor ) ) continue;
            e.value = drag_.value;
        }
    }
    std::sort( out.begin(), out.end() );
    return out;
}

void SPianoRollView::commitDrag()
{
    const DragKind kind = drag_.kind;
    if( kind == DragKind::None ) return;

    const Resolved r = resolve();
    if( !r.valid() ) { drag_ = Drag(); update(); return; }

    if( kind == DragKind::Marquee ) {
        const QRect m = QRect( drag_.press, drag_.cur ).normalized();
        for( const SEvent &e : notesOf( r ) )
            if( noteRect( r, e ).intersects( m ) ) selected_.insert( idStr( e ) );
        drag_ = Drag();
        update();
        return;
    }

    if( kind == DragKind::Cc ) {
        if( drag_.ccLane >= 0 && drag_.ccLane < ccLanes_.size() && r.seq ) {
            const int cc = ccLanes_.at( drag_.ccLane );
            const qint64 t = snapTick( r, tickOfX( r, drag_.cur.x() - KEYS_W ) );
            // set-events is ABSOLUTE over the whole table, so the new state is
            // "everything, minus the point being replaced, plus the new one".
            std::vector<SEvent> events = r.seq->events();
            events.erase(
                std::remove_if( events.begin(), events.end(),
                                [&]( const SEvent &e ) {
                                    return e.kind == twEventKind::ControlChange
                                        && (int) e.paramId == cc && e.t == t;
                                } ),
                events.end() );
            SEvent e;
            e.t = t;
            e.kind = twEventKind::ControlChange;
            e.channel = 0;
            e.paramId = (quint32) cc;
            e.value = drag_.value;
            events.push_back( e );
            std::sort( events.begin(), events.end() );
            SApplication::app().submitAction(
                new SSetEventsAction( clipPath_, events, -1 ) );
        }
        drag_ = Drag();
        update();
        return;
    }

    // Move / Resize / Velocity: the preview is DISCARDED and the resulting
    // absolute state is submitted as ONE set-notes (revert-then-action). The
    // model never saw an intermediate position, so there is nothing to undo
    // but the gesture itself.
    const std::vector<SEvent> notes = notesOf( r );
    const std::vector<SEvent> next = previewNotes( r, notes );
    const Drag done = drag_;
    drag_ = Drag();

    if( !sameNotes( next, notes ) ) {
        // Follow the selection to the new ADDRESSES before the action lands, so
        // the arrangementChanged refresh does not prune it away.
        const bool wholeSelection = selected_.contains( done.anchor );
        QSet<QString> keep;
        for( const SEvent &e : next ) {
            const QString id = idStr( e );
            if( wholeSelection || id == done.anchor ) keep.insert( id );
        }
        if( !wholeSelection ) {
            // A single-note gesture moved exactly one address; everything else
            // kept its own, so the previous selection still resolves.
            keep.unite( selected_ );
        }
        selected_ = keep;
        commitNotes( r, next );
    }
    update();
}

void SPianoRollView::selectOnly( const QString &id )
{
    selected_.clear();
    selected_.insert( id );
}

void SPianoRollView::nudgeSelection( qint64 dTicks, int dKey )
{
    const Resolved r = resolve();
    if( !r.valid() || selected_.isEmpty() ) return;
    std::vector<SEvent> notes = notesOf( r );
    QSet<QString> moved;
    for( SEvent &e : notes ) {
        if( !selected_.contains( idStr( e ) ) ) continue;
        e.t = std::max<qint64>( 0, e.t + dTicks );
        e.key = std::min( 127, std::max( 0, e.key + dKey ) );
        moved.insert( idStr( e ) );
    }
    std::sort( notes.begin(), notes.end() );
    selected_ = moved;
    commitNotes( r, notes );
}

void SPianoRollView::keyPressEvent( QKeyEvent *ev )
{
    // Space belongs to the transport, always (proposal 37 6.3). Anything this
    // view does not consume is ignore()d and propagates, which is what keeps
    // the shell's shortcuts alive while the editor holds focus.
    const Resolved r = resolve();
    const qint64 g = r.valid() ? gridTicks( r ) : 0;
    switch( ev->key() ) {
    case Qt::Key_Left:   nudgeSelection( -( g > 0 ? g : 240 ), 0 ); break;
    case Qt::Key_Right:  nudgeSelection(  ( g > 0 ? g : 240 ), 0 ); break;
    case Qt::Key_Up:     nudgeSelection( 0,  1 ); break;
    case Qt::Key_Down:   nudgeSelection( 0, -1 ); break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace: {
        if( !r.valid() || selected_.isEmpty() ) { ev->ignore(); return; }
        const std::vector<SEvent> notes = notesOf( r );
        std::vector<SEvent> keep;
        for( const SEvent &e : notes )
            if( !selected_.contains( idStr( e ) ) ) keep.push_back( e );
        selected_.clear();
        commitNotes( r, keep );
        break;
    }
    default:
        ev->ignore();
        return;
    }
    ev->accept();
}

void SPianoRollView::resizeEvent( QResizeEvent *ev )
{
    QWidget::resizeEvent( ev );
    layoutKeyScroll();
    update();
}

// ---------------------------------------------------------------------------
// The test entry point (`drag-note`)
// ---------------------------------------------------------------------------

bool SPianoRollView::tkDragNote( qint64 tick, int key, int channel,
                                 qint64 toTick, int toKey, const QString &edge,
                                 const QString &lane, double toValue )
{
    const Resolved r = resolve();
    if( !r.valid() || !axis_ ) return false;

    const std::vector<SEvent> notes = notesOf( r );
    const SEvent *hit = nullptr;
    for( const SEvent &e : notes ) {
        if( e.t != tick || e.key != key ) continue;
        if( channel >= 0 && e.channel != channel ) continue;
        hit = &e;
        break;
    }
    if( !hit ) return false;

    const bool velocityLane = ( lane == QLatin1String( "velocity" ) );
    const bool resizeEnd    = ( edge == QLatin1String( "end" ) );

    const int xStart = xOfTick( r, hit->t ) + KEYS_W;
    const int xEnd   = xOfTick( r, hit->t + ( hit->dur > 0 ? hit->dur : 1 ) )
                     + KEYS_W;
    const int xTo    = xOfTick( r, toTick ) + KEYS_W;
    if( xStart < 0 || xTo < 0 ) return false;

    // The view is never SHOWN in a headless run, so it has whatever size the
    // layout last gave it. Grow it until both ends of the gesture are inside:
    // the handlers work off the axis, not off screen geometry, so this changes
    // nothing but the hit-testable area (the trick dragClipEdge uses).
    const int destKey = ( velocityLane || resizeEnd ) ? hit->key : toKey;
    const int lowKey  = std::min( hit->key, destKey );
    const int keyRows = std::max( 1, ( topKey_ - lowKey ) + 3 );
    const int needH   = keyRows * keyHeight_ + VEL_H
                      + ccLanes_.size() * CC_H + 8;
    const int needW   = std::max( std::max( xStart, xEnd ), xTo ) + 64;
    if( height() < needH || width() < needW )
        resize( std::max( width(), needW ), std::max( height(), needH ) );

    int x0, x1, y0, y1;
    if( velocityLane ) {
        x0 = x1 = xStart;
        y0 = velTop() + VEL_H
           - (int) ( VEL_H * clampVel( hit->value ) / 127.0 ) + 1;
        y1 = velTop() + VEL_H - (int) ( VEL_H * clampVel( toValue ) / 127.0 );
        if( y0 < velTop() )              y0 = velTop();
        if( y0 >= velTop() + VEL_H )     y0 = velTop() + VEL_H - 1;
        if( y1 < velTop() )              y1 = velTop();
        if( y1 >= velTop() + VEL_H )     y1 = velTop() + VEL_H - 1;
    } else {
        // A press must land clear of the end grab band for a MOVE, and inside
        // it for a RESIZE - the same two-band rule drag-clip-edge follows.
        x0 = resizeEnd ? xEnd - 1
                       : std::min( xStart + 1, ( xStart + xEnd ) / 2 );
        if( x0 < xStart ) x0 = xStart;
        x1 = resizeEnd ? xTo : xTo + ( x0 - xStart );
        y0 = yOfKey( hit->key ) + keyHeight_ / 2;
        y1 = yOfKey( destKey ) + keyHeight_ / 2;
        if( y0 < 0 || y1 < 0 ) return false;
    }

    // Move/resize are Select-tool gestures whatever the toolbar says: the verb
    // names the gesture, not the tool.
    const Tool saved = tool_;
    tool_ = Tool::Select;

    auto send = [&]( QEvent::Type type, int x, int y, Qt::MouseButton button,
                     Qt::MouseButtons buttons ) {
        const QPointF local( x, y );
        QMouseEvent ev( type, local, mapToGlobal( local ), button, buttons,
                        Qt::NoModifier );
        QApplication::sendEvent( this, &ev );
    };
    send( QEvent::MouseButtonPress,   x0, y0, Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseMove,          x1, y1, Qt::NoButton,   Qt::LeftButton );
    send( QEvent::MouseButtonRelease, x1, y1, Qt::LeftButton, Qt::NoButton );

    tool_ = saved;
    return true;
}

int SPianoRollView::tkScrollKeys( int topKey )
{
    if( !keyScroll_ ) return topKey_;

    const int rows = std::max( 1, gridHeight() / keyHeight_ );
    const int minTopKey = rows - 1;   // the lowest topKey_ that still shows key 0
    const int clamped = std::max( minTopKey, std::min( 127, topKey ) );
    const int maxV = std::max( 0, 128 - rows );
    const int v = std::max( 0, std::min( maxV, 127 - clamped ) );

    // The REAL scrollbar under test, not a direct topKey_ assignment: this is
    // the same call a dragged handle makes, so its valueChanged lambda is
    // what actually moves topKey_ (a no-op setValue, when v already matches,
    // means topKey_ was already correct - see layoutKeyScroll()).
    keyScroll_->setValue( v );
    return topKey_;
}

// The KIND REGISTRY entry (proposal 37 6.2). A static initializer, which is why
// app_ui must stay an OBJECT library - a STATIC one would drop this silently.
static const bool s_reg_pianoroll = (
    SEventEditorRegistry::instance().registerKind(
        QStringLiteral( "pianoroll" ), QStringLiteral( "Piano Roll" ),
        []( QWidget *parent ) -> SEventEditorView * {
            return new SPianoRollView( parent );
        } ),
    true
);
