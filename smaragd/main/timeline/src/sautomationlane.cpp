#include "sautomationlane.h"

#include "app/timeline/sstdmixerview.h"
#include "app/timeline/ssubmit.h"
#include "app/timeline/sfadercurve.h"

#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/sautomationactions.h"
#include "app/objects/track/strack.h"
#include "app/shell/sapplication.h"
#include "app/shell/sautomationrecorder.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

// How close (px) the pointer has to be to grab an existing breakpoint.
static const int AUTO_GRAB_PX = 5;
// Half-side of the painted breakpoint marker.
static const int AUTO_POINT_R = 3;

// ---------------------------------------------------------------------------
// The value scale
// ---------------------------------------------------------------------------

double SAutoValueScale::toNorm( double v ) const
{
    double n;
    if( fader ) {
        // THE fader curve (timeline inv. 13), so a lane and the head's fader
        // put the same dB at the same fraction of their travel.
        const double f = (double) sDbToFader( v );
        n = ( f - (double) SFADER_MIN ) / (double) ( SFADER_MAX - SFADER_MIN );
    } else {
        const double span = hi - lo;
        n = ( span > 0.0 ) ? ( v - lo ) / span : 0.0;
    }
    if( n < 0.0 ) n = 0.0;
    if( n > 1.0 ) n = 1.0;
    return n;
}

double SAutoValueScale::fromNorm( double n ) const
{
    if( n < 0.0 ) n = 0.0;
    if( n > 1.0 ) n = 1.0;
    if( fader ) {
        const int f = (int) ( (double) SFADER_MIN
                              + n * (double) ( SFADER_MAX - SFADER_MIN ) + 0.5 );
        return sFaderToDb( f );
    }
    double v = lo + n * ( hi - lo );
    if( stepped ) v = ( v >= 0.5 * ( lo + hi ) ) ? hi : lo;
    return v;
}

SAutoValueScale sAutoScaleFor( const QString &target, SObject *owner )
{
    SAutoValueScale s;
    const SParamRef ref = SParamRef::parse( target );
    switch( ref.space ) {
    case SParamRef::Space::Self:
        if( ref.prop == QLatin1String( "Muted" ) ) {
            s.lo = 0.0; s.hi = 1.0; s.stepped = true;
        } else {                                   // Volume
            s.lo = SFADER_MIN_DB; s.hi = SFADER_MAX_DB; s.fader = true;
        }
        break;
    case SParamRef::Space::Param: {
        // The plugin's OWN declared range - normalized [0,1] for VST3, native
        // for CLAP/AU. Asking the slot rather than assuming [0,1] is what makes
        // a CLAP cutoff in Hz drawable at all.
        s.lo = 0.0; s.hi = 1.0;
        if( SPluginSlot *slot = dynamic_cast<SPluginSlot *>( owner ) ) {
            const QVector<SPluginSlot::ParamRow> rows = slot->paramRows();
            for( const SPluginSlot::ParamRow &r : rows ) {
                if( r.id != ref.paramId ) continue;
                if( r.maxValue > r.minValue ) { s.lo = r.minValue; s.hi = r.maxValue; }
                s.stepped = r.isStepped;
                break;
            }
        }
        break;
    }
    case SParamRef::Space::Cut:
        if( ref.prop == QLatin1String( "Transpose" ) )           { s.lo = -24.0; s.hi = 24.0; }
        else if( ref.prop == QLatin1String( "VelocityScale" ) )  { s.lo = 0.0;   s.hi = 2.0; }
        else                                                     { s.lo = 0.0;   s.hi = 1.0; }
        break;
    default:
        break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

namespace {

SProject *projectOf()
{
    return SApplication::app().getCurrentProject();
}

// Every track under `container`, including collapsed and nested ones. The ROWS
// are not enough: a collapsed folder's children are alive but have no row, and
// pruning against the rows would forget their UI state on every fold.
void collectTracks( SObject *container, QSet<const STrack *> &out )
{
    if( !container ) return;
    for( SLink *lk : container->childLinks() ) {
        STrack *tk = dynamic_cast<STrack *>( &lk->getSObject() );
        if( !tk ) continue;
        out.insert( tk );
        collectTracks( tk, out );
    }
}

// A short human label for a target, for the lane's own caption.
QString labelFor( const QString &target, SObject *owner )
{
    const SParamRef ref = SParamRef::parse( target );
    if( ref.space == SParamRef::Space::Param ) {
        if( SPluginSlot *slot = dynamic_cast<SPluginSlot *>( owner ) ) {
            const QVector<SPluginSlot::ParamRow> rows = slot->paramRows();
            for( const SPluginSlot::ParamRow &r : rows )
                if( r.id == ref.paramId && !r.name.isEmpty() ) return r.name;
            if( SAutomationLane *l = slot->automationLane( target ) )
                if( !l->paramName().isEmpty() ) return l->paramName();
        }
        return QStringLiteral( "Param %1" ).arg( ref.paramId );
    }
    return ref.prop.isEmpty() ? target : ref.prop;
}

}  // namespace

// ---------------------------------------------------------------------------
// SAutomationLaneUi
// ---------------------------------------------------------------------------

SAutomationLaneUi::SAutomationLaneUi( SStdMixerView &view ) : view_( view ) {}
SAutomationLaneUi::~SAutomationLaneUi() = default;

const QVector<SAutoLaneRef> &SAutomationLaneUi::shownLanes( const STrack *t ) const
{
    static const QVector<SAutoLaneRef> kEmpty;
    auto it = shown_.constFind( t );
    return it == shown_.constEnd() ? kEmpty : it.value();
}

bool SAutomationLaneUi::isLaneShown( const STrack *t, const SAutoLaneRef &r ) const
{
    return shownLanes( t ).contains( r );
}

void SAutomationLaneUi::toggleLane( STrack *t, const SAutoLaneRef &r )
{
    if( !t || !r.isValid() ) return;
    QVector<SAutoLaneRef> &v = shown_[t];
    const int i = v.indexOf( r );
    if( i >= 0 ) v.remove( i );
    else         v.append( r );
    if( v.isEmpty() ) shown_.remove( t );
    view_.refreshTrackTree();
}

void SAutomationLaneUi::pruneTo( const QSet<const STrack *> &live )
{
    for( auto it = shown_.begin(); it != shown_.end(); ) {
        if( live.contains( it.key() ) ) ++it;
        else                            it = shown_.erase( it );
    }
}

// --- resolution -------------------------------------------------------------

SAutomationLaneUi::Hit
SAutomationLaneUi::resolveRow( const STrackRow &row, const QRect &laneRect ) const
{
    Hit h;
    h.rect = laneRect;
    h.ref.target = row.autoTarget;
    h.ref.slotIndex = row.autoSlotIndex;
    SProject *proj = projectOf();
    SStdMixer *mixer = view_.getModel();
    if( !proj || !mixer || !row.track || row.autoTarget.isEmpty() ) return h;
    h.ownerPath = strackpath::pathOf( mixer, row.track );
    sautomation::OwnerRef o = sautomation::resolveOwner( proj, sautomation::rootNameOf( proj, mixer ), h.ownerPath, row.autoTarget, row.autoSlotIndex, -1 );
    if( !o.valid() ) return h;
    h.owner = o.owner;
    h.lane  = o.owner->automationLane( row.autoTarget );
    h.scale = sAutoScaleFor( row.autoTarget, o.owner );
    return h;
}

int SAutomationLaneUi::pointAt( const Hit &h, SMVActualView &view,
                                const QPoint &pos ) const
{
    if( !h.valid() ) return -1;
    const std::vector<SAutomationPoint> pts = h.lane->points();
    const int bottom = h.rect.bottom();
    const int span = h.rect.height() - 1;
    if( span < 1 ) return -1;
    for( std::size_t i = 0; i < pts.size(); ++i ) {
        const int px = view.getXPosOfOffset( pts[i].frame );
        const int py = bottom - (int) ( h.scale.toNorm( pts[i].value ) * span + 0.5 );
        if( std::abs( px - pos.x() ) <= AUTO_GRAB_PX
            && std::abs( py - pos.y() ) <= AUTO_GRAB_PX )
            return (int) i;
    }
    return -1;
}

// --- painting ---------------------------------------------------------------

void SAutomationLaneUi::drawAutomationLane( QPainter &p, SMVActualView &view,
                                            const STrackRow &row,
                                            const QRect &laneRect )
{
    p.fillRect( laneRect, QColor( 22, 32, 44 ) );
    const Hit h = resolveRow( row, laneRect );

    // The value axis: floor, middle, ceiling. Three lines, because a curve with
    // no reference is a squiggle - "where is unity" is the first question a
    // clip-gain or a fader envelope has to answer.
    p.setPen( QColor( 52, 66, 82 ) );
    for( int k = 0; k <= 2; ++k ) {
        const int y = laneRect.bottom() - ( laneRect.height() - 1 ) * k / 2;
        p.drawLine( laneRect.left(), y, laneRect.right(), y );
    }

    // The caption, left-aligned in the lane, so several lanes on one track are
    // told apart without consulting the picker.
    p.setPen( QColor( 150, 170, 190 ) );
    p.drawText( laneRect.adjusted( 4, 1, -4, 0 ), Qt::AlignLeft | Qt::AlignTop,
                labelFor( row.autoTarget, h.owner ) );

    if( !h.valid() ) return;

    const std::vector<SAutomationPoint> pts = h.lane->points();
    const int bottom = h.rect.bottom();
    const int span = h.rect.height() - 1;
    if( span < 1 ) return;

    // THE CURVE, sampled per PIXEL through SAutomationLane::valueAt - the same
    // call assert-automation-value makes. Step / Linear / Exp therefore come
    // out right by construction; a per-segment painter would be a second
    // implementation of the interpolation and could disagree with the ear.
    const bool live = SAutomationRecorder::isReadFamily( h.lane->mode() );
    p.setPen( QPen( live ? QColor( 240, 190, 60 ) : QColor( 130, 130, 130 ),
                    live ? 2 : 1 ) );
    QVector<QPoint> poly;
    poly.reserve( laneRect.width() + 1 );
    for( int x = laneRect.left(); x <= laneRect.right(); ++x ) {
        const offset_t t = view.getTimeOf( x );
        const double v = h.lane->valueAt( t < 0 ? 0 : t );
        poly.append( QPoint( x, bottom - (int) ( h.scale.toNorm( v ) * span + 0.5 ) ) );
    }
    if( poly.size() >= 2 ) p.drawPolyline( poly.constData(), poly.size() );

    // The breakpoints on top of it, selected ones filled white.
    for( const SAutomationPoint &pt : pts ) {
        const int px = view.getXPosOfOffset( pt.frame );
        if( px < laneRect.left() - AUTO_POINT_R
            || px > laneRect.right() + AUTO_POINT_R ) continue;
        const int py = bottom - (int) ( h.scale.toNorm( pt.value ) * span + 0.5 );
        const QRect r( px - AUTO_POINT_R, py - AUTO_POINT_R,
                       2 * AUTO_POINT_R + 1, 2 * AUTO_POINT_R + 1 );
        const bool sel = ( dragRef_ == h.ref )
                      && selected_.contains( (qint64) pt.frame );
        p.fillRect( r, sel ? QColor( 255, 255, 255 ) : QColor( 255, 220, 120 ) );
        p.setPen( QColor( 40, 30, 10 ) );
        p.drawRect( r );
    }

    // The marquee, while one is being dragged over THIS lane.
    if( drag_ == Drag::Marquee && dragRef_ == h.ref ) {
        p.setPen( QColor( 200, 220, 255 ) );
        p.drawRect( QRect( marqueeFrom_, marqueeTo_ ).normalized() );
    }
}

// --- the picker -------------------------------------------------------------

void SAutomationLaneUi::buildPickerMenu( QMenu *parent, STrack *t )
{
    if( !parent || !t ) return;
    QMenu *m = parent->addMenu( QObject::tr( "Show &automation" ) );

    auto addEntry = [&]( const QString &label, const QString &target, int slot ) {
        QAction *a = m->addAction( label );
        a->setCheckable( true );
        SAutoLaneRef ref; ref.target = target; ref.slotIndex = slot;
        a->setChecked( isLaneShown( t, ref ) );
        QObject::connect( a, &QAction::triggered, m,
                          [this, t, ref]() { toggleLane( t, ref ); } );
    };

    addEntry( QObject::tr( "&Volume" ), QStringLiteral( "self:Volume" ), -1 );
    addEntry( QObject::tr( "&Mute" ),   QStringLiteral( "self:Muted" ),  -1 );

    // Every parameter of every slot on the track. A plugin that is missing on
    // this machine reports no rows, so it contributes no entries rather than a
    // list of numbers nobody can map back to a knob.
    if( SPluginChain *chain = t->getPluginChain() ) {
        for( int i = 0; i < chain->getSlotCount(); ++i ) {
            SPluginSlot *slot = chain->getSlotAt( i );
            if( !slot ) continue;
            const QVector<SPluginSlot::ParamRow> rows = slot->paramRows();
            if( rows.isEmpty() ) continue;
            QString sname = QString::fromStdString( slot->getDescriptor().name );
            if( sname.isEmpty() ) sname = QObject::tr( "Slot %1" ).arg( i );
            QMenu *sm = m->addMenu( sname );
            for( const SPluginSlot::ParamRow &r : rows ) {
                QAction *a = sm->addAction(
                    r.name.isEmpty() ? QStringLiteral( "Param %1" ).arg( r.id )
                                     : r.name );
                a->setCheckable( true );
                SAutoLaneRef ref;
                ref.target = QStringLiteral( "param:%1" ).arg( r.id );
                ref.slotIndex = i;
                a->setChecked( isLaneShown( t, ref ) );
                QObject::connect( a, &QAction::triggered, sm,
                                  [this, t, ref]() { toggleLane( t, ref ); } );
            }
        }
    }

    m->addSeparator();
    QAction *env = m->addAction( QObject::tr( "Edit clip &envelopes" ) );
    env->setCheckable( true );
    env->setChecked( envelopeEdit_ );
    QObject::connect( env, &QAction::triggered, m, [this]( bool on ) {
        setEnvelopeEditEnabled( on );
        view_.contentView()->update();
    } );
}

// --- the clip envelope (cut:Gain) -------------------------------------------

SAutomationLaneUi::Hit
SAutomationLaneUi::resolveClipEnvelope( SMVActualView &view, int rowIdx,
                                        const QPoint &pos ) const
{
    Hit h;
    if( !envelopeEdit_ ) return h;
    const STrackRow *row = view_.rowAt( rowIdx );
    SProject *proj = projectOf();
    SStdMixer *mixer = view_.getModel();
    if( !row || row->isSubLane() || !row->track || !proj || !mixer ) return h;

    // The clip whose span contains the pointer. Nested tracks are lanes, not
    // clips, so they are skipped exactly as dragClipEdge skips them.
    int idx = -1, n = 0;
    SLink *hitLink = nullptr;
    for( SLink *lk : row->track->childLinks() ) {
        if( dynamic_cast<STrack *>( &lk->getSObject() ) ) continue;
        const int i = n++;
        if( !lk->hasStartTime() || !lk->getSObject().hasDuration() ) continue;
        const offset_t s = lk->getStartTime();
        const offset_t e = s + (offset_t) lk->getSObject().getDuration();
        if( pos.x() >= view.getXPosOfOffset( s )
            && pos.x() < view.getXPosOfOffset( e ) ) { hitLink = lk; idx = i; break; }
    }
    if( !hitLink ) return h;

    h.ownerPath = strackpath::pathOf( mixer, row->track );
    h.ownerPath.append( idx );
    h.ref.target = QStringLiteral( "cut:Gain" );
    h.ref.slotIndex = -1;
    sautomation::OwnerRef o = sautomation::resolveOwner( proj, sautomation::rootNameOf( proj, mixer ), h.ownerPath, h.ref.target, -1, -1 );
    if( !o.valid() ) return h;
    h.owner = o.owner;
    h.lane  = o.owner->ensureAutomationLane( h.ref.target );
    h.scale = sAutoScaleFor( h.ref.target, o.owner );
    h.timeBase = hitLink->getStartTime();
    // The envelope is drawn on the CLIP, so the grab band is the clip's own
    // rectangle within the lane.
    const int top = view.laneTop( rowIdx ) + 1;
    const int hgt = view.laneHeight( rowIdx ) - 2;
    const int x0 = view.getXPosOfOffset( hitLink->getStartTime() );
    const int x1 = view.getXPosOfOffset(
        hitLink->getStartTime() + (offset_t) hitLink->getSObject().getDuration() );
    h.rect = QRect( x0, top, x1 - x0, hgt );
    return h;
}

// --- gestures ----------------------------------------------------------------

void SAutomationLaneUi::beginPointDrag( const Hit &h, int index )
{
    const std::vector<SAutomationPoint> pts = h.lane->points();
    if( index < 0 || index >= (int) pts.size() ) return;
    drag_            = Drag::Point;
    dragOwnerPath_   = h.ownerPath;
    dragRef_         = h.ref;
    dragTake_        = h.take;
    dragLane_        = h.lane;
    dragScale_       = h.scale;
    dragRect_        = h.rect;
    dragBase_        = h.timeBase;
    dragUndoPoints_  = pts;
    dragFromTime_    = pts[index].frame;
    dragFromValue_   = pts[index].value;
    dragToTime_      = dragFromTime_;
    dragToValue_     = dragFromValue_;
    dragTension_     = pts[index].tension;
}

void SAutomationLaneUi::applyLivePoint()
{
    if( !dragLane_ ) return;
    std::vector<SAutomationPoint> pts = dragUndoPoints_;
    for( SAutomationPoint &p : pts ) {
        if( p.frame != dragFromTime_ ) continue;
        if( drag_ == Drag::Tension ) {
            p.shape   = ( std::fabs( dragTension_ ) < 1e-9 ) ? twCurveShape::Linear
                                                             : twCurveShape::Exp;
            p.tension = dragTension_;
        } else {
            p.frame = dragToTime_;
            p.value = dragToValue_;
        }
        break;
    }
    // Live PREVIEW only: the table moves so the user sees the curve follow the
    // pointer, but nothing is pushed into the engine and nothing lands on the
    // undo stack. The release REVERTS this and commits through a verb
    // (timeline inv. 3).
    dragLane_->setPoints( std::move( pts ) );
}

bool SAutomationLaneUi::press( SMVActualView &view, int rowIdx,
                               const QPoint &pos, Qt::KeyboardModifiers mods,
                               bool rightButton )
{
    consumed_ = false;
    if( rightButton || drag_ != Drag::None ) return false;
    const STrackRow *row = view_.rowAt( rowIdx );
    if( !row ) return false;

    consumed_ = true;   // cleared again below if the row is not ours
    Hit h;
    if( row->subKind == SubLaneKind::Automation ) {
        h = resolveRow( *row, QRect( 0, view.laneTop( rowIdx ) + 1,
                                     view.width(),
                                     view.laneHeight( rowIdx ) - 2 ) );
        // The row is OURS: an unresolvable owner must be swallowed here rather
        // than falling through to the clip gestures on a lane that has none.
        if( !h.owner ) return true;
        if( !h.lane ) h.lane = h.owner->ensureAutomationLane( h.ref.target );
    } else {
        h = resolveClipEnvelope( view, rowIdx, pos );
        if( !h.valid() ) { consumed_ = false; return false; }   // the clip
                                                                // gestures own it
    }
    if( !h.valid() ) return true;

    const int idx = pointAt( h, view, pos );
    const bool primary = mods.testFlag( Qt::ControlModifier )
                      || mods.testFlag( Qt::MetaModifier );

    // Primary-click on a point DELETES it. Alt is already the tension gesture
    // and Shift is the marquee, so the delete takes the remaining modifier.
    if( idx >= 0 && primary ) {
        const std::vector<SAutomationPoint> pts = h.lane->points();
        stimeline::submitActive(
            new SRemoveAutomationPointAction( h.ownerPath, h.ref.target,
                                              pts[idx].frame, pts[idx].value,
                                              h.ref.slotIndex, h.take ) );
        view.update();
        return true;
    }

    if( idx >= 0 && mods.testFlag( Qt::AltModifier ) ) {
        beginPointDrag( h, idx );
        drag_ = Drag::Tension;
        return true;
    }

    if( idx < 0 && mods.testFlag( Qt::ShiftModifier ) ) {
        drag_          = Drag::Marquee;
        dragOwnerPath_ = h.ownerPath;
        dragRef_       = h.ref;
        dragTake_      = h.take;
        dragLane_      = h.lane;
        dragScale_     = h.scale;
        dragRect_      = h.rect;
        dragBase_      = h.timeBase;
        marqueeFrom_   = pos;
        marqueeTo_     = pos;
        selected_.clear();
        view.update();
        return true;
    }

    if( idx < 0 ) {
        // A CLICK ON EMPTY LANE ADDS A POINT - one action, one undo step. The
        // drag that may follow commits a `move-automation-point` only when the
        // point actually moved, so a plain click stays exactly one step.
        offset_t t = view_.alignTime( view.getTimeOf( pos.x() ) );
        if( t < h.timeBase ) t = h.timeBase;
        const offset_t rel = t - h.timeBase;
        const int span = h.rect.height() - 1;
        const double v = h.scale.fromNorm(
            span > 0 ? ( h.rect.bottom() - pos.y() ) / (double) span : 0.0 );
        stimeline::submitActive(
            new SAddAutomationPointAction( h.ownerPath, h.ref.target, rel, v,
                                           h.scale.stepped ? twCurveShape::Step
                                                           : twCurveShape::Linear,
                                           0.0, h.ref.slotIndex, h.take ) );
        const std::vector<SAutomationPoint> pts = h.lane->points();
        for( std::size_t i = 0; i < pts.size(); ++i )
            if( pts[i].frame == rel ) { beginPointDrag( h, (int) i ); break; }
        view.update();
        return true;
    }

    beginPointDrag( h, idx );
    selected_.clear();
    selected_.append( (qint64) dragFromTime_ );
    view.update();
    return true;
}

bool SAutomationLaneUi::move( SMVActualView &view, const QPoint &pos )
{
    if( !consumed_ ) return false;
    if( drag_ == Drag::None ) return true;
    if( drag_ == Drag::Marquee ) { marqueeTo_ = pos; view.update(); return true; }
    if( !dragLane_ ) return true;

    const int span = dragRect_.height() - 1;
    if( drag_ == Drag::Tension ) {
        // Vertical travel from the point BENDS the segment: up is late, down is
        // early, and the middle is exactly linear.
        const double dy = ( dragRect_.bottom() - pos.y() ) / (double) qMax( 1, span );
        dragTension_ = ( dy - dragScale_.toNorm( dragFromValue_ ) ) * 8.0;
        if( dragTension_ >  8.0 ) dragTension_ =  8.0;
        if( dragTension_ < -8.0 ) dragTension_ = -8.0;
    } else {
        // The drag is expressed in the OWNER's domain; a clip envelope's is
        // clip-relative, which is what dragBase_ subtracts.
        offset_t t = view_.alignTime( view.getTimeOf( pos.x() ) );
        offset_t rel = t - dragBase_;
        if( rel < 0 ) rel = 0;
        dragToTime_  = rel;
        dragToValue_ = dragScale_.fromNorm(
            span > 0 ? ( dragRect_.bottom() - pos.y() ) / (double) span : 0.0 );
    }
    applyLivePoint();
    view.update();
    return true;
}

bool SAutomationLaneUi::release( SMVActualView &view, const QPoint &pos )
{
    if( !consumed_ ) return false;
    consumed_ = false;
    if( drag_ == Drag::None ) return true;
    const Drag what = drag_;
    drag_ = Drag::None;

    if( what == Drag::Marquee ) {
        marqueeTo_ = pos;
        selected_.clear();
        if( dragLane_ ) {
            const QRect m = QRect( marqueeFrom_, marqueeTo_ ).normalized();
            const int span = dragRect_.height() - 1;
            const std::vector<SAutomationPoint> pts = dragLane_->points();
            for( const SAutomationPoint &p : pts ) {
                const int px = view.getXPosOfOffset( p.frame + dragBase_ );
                const int py = dragRect_.bottom()
                             - (int) ( dragScale_.toNorm( p.value ) * span + 0.5 );
                if( m.contains( px, py ) ) selected_.append( (qint64) p.frame );
            }
        }
        view.update();
        return true;
    }

    if( !dragLane_ ) return true;

    // REVERT, THEN ACT (timeline inv. 3): the live preview mutated the table
    // directly, so the model has to go back to what it was - otherwise the
    // ACTION would find nothing to change, its undo step would be a no-op and
    // a redo would double-apply.
    dragLane_->setPoints( dragUndoPoints_ );

    if( what == Drag::Tension ) {
        SAutomationPoint p;
        for( const SAutomationPoint &q : dragUndoPoints_ )
            if( q.frame == dragFromTime_ ) { p = q; break; }
        p.shape   = ( std::fabs( dragTension_ ) < 1e-9 ) ? twCurveShape::Linear
                                                         : twCurveShape::Exp;
        p.tension = dragTension_;
        std::vector<SAutomationPoint> one{ p };
        stimeline::submitActive(
            new SSetAutomationPointsAction( dragOwnerPath_, dragRef_.target,
                                            dragFromTime_, dragFromTime_ + 1,
                                            std::move( one ), dragRef_.slotIndex,
                                            dragTake_ ) );
        view.update();
        return true;
    }

    if( dragToTime_ != dragFromTime_ || dragToValue_ != dragFromValue_ ) {
        stimeline::submitActive(
            new SMoveAutomationPointAction( dragOwnerPath_, dragRef_.target,
                                            dragFromTime_, dragFromValue_,
                                            dragToTime_, dragToValue_,
                                            dragRef_.slotIndex, dragTake_ ) );
        selected_.clear();
        selected_.append( (qint64) dragToTime_ );
    }
    view.update();
    return true;
}

bool SAutomationLaneUi::deleteSelection()
{
    if( !dragLane_ || selected_.isEmpty() ) return false;
    qint64 lo = selected_.first(), hi = selected_.first();
    for( qint64 f : selected_ ) { lo = qMin( lo, f ); hi = qMax( hi, f ); }

    // ONE action for the whole selection: the batch verb over exactly the
    // window the selection spans, carrying the survivors.
    std::vector<SAutomationPoint> keep;
    const std::vector<SAutomationPoint> pts = dragLane_->points();
    for( const SAutomationPoint &p : pts ) {
        if( p.frame < (offset_t) lo || p.frame > (offset_t) hi ) continue;
        if( selected_.contains( (qint64) p.frame ) ) continue;
        keep.push_back( p );
    }
    stimeline::submitActive(
        new SSetAutomationPointsAction( dragOwnerPath_, dragRef_.target,
                                        (offset_t) lo, (offset_t) hi + 1,
                                        std::move( keep ), dragRef_.slotIndex,
                                        dragTake_ ) );
    selected_.clear();
    view_.contentView()->update();
    return true;
}

// --- the testkit driver -------------------------------------------------------

bool SAutomationLaneUi::tkDrag( const QList<int> &ownerPath, const QString &target,
                                int slotIndex, int take, offset_t time,
                                double value, offset_t toTime, double toValue,
                                Qt::KeyboardModifiers mods )
{
    SMVActualView *canvas = view_.contentView();
    SProject *proj = projectOf();
    SStdMixer *mixer = view_.getModel();
    if( !canvas || !proj || !mixer ) return false;

    sautomation::OwnerRef o =
        sautomation::resolveOwner( proj, sautomation::rootNameOf( proj, mixer ), ownerPath, target, slotIndex, take );
    if( !o.valid() ) return false;
    const SAutoValueScale scale = sAutoScaleFor( target, o.owner );
    const SParamRef ref = SParamRef::parse( target );

    // Find the ROW the gesture has to land on, and the time base of its domain.
    int rowIdx = -1;
    offset_t base = 0;
    QRect rect;
    if( ref.space == SParamRef::Space::Cut ) {
        // A clip envelope: the gesture lands on the TRACK lane that carries the
        // placement, and only while envelope editing is armed.
        if( !envelopeEdit_ ) {
            qWarning() << "drag-automation-point: a cut: target needs clip"
                          " envelope editing armed";
            return false;
        }
        QList<int> trackPath = ownerPath;
        if( trackPath.isEmpty() ) return false;
        trackPath.removeLast();
        SLink *link = splacements::placementAt(
            splacements::rootContainer( proj ), ownerPath );
        STrack *track = dynamic_cast<STrack *>(
            splacements::laneAt( splacements::rootContainer( proj ), trackPath ) );
        if( !link || !track ) return false;
        rowIdx = view_.rowIndexOfTrack( track );
        base = link->getStartTime();
        if( rowIdx < 0 ) return false;
        const int x0 = canvas->getXPosOfOffset( base );
        const int x1 = canvas->getXPosOfOffset(
            base + (offset_t) link->getSObject().getDuration() );
        rect = QRect( x0, canvas->laneTop( rowIdx ) + 1, x1 - x0,
                      canvas->laneHeight( rowIdx ) - 2 );
    } else {
        for( int i = 0; i < view_.rowCount(); ++i ) {
            const STrackRow *r = view_.rowAt( i );
            if( !r || r->subKind != SubLaneKind::Automation ) continue;
            if( r->autoTarget != target || r->autoSlotIndex != slotIndex ) continue;
            const QList<int> p = strackpath::pathOf( mixer, r->track );
            if( p != ownerPath ) continue;
            rowIdx = i;
            break;
        }
        if( rowIdx < 0 ) {
            qWarning() << "drag-automation-point: no lane row for" << target
                       << "on" << strackpath::pathToString( ownerPath )
                       << "(is the lane shown?)";
            return false;
        }
        rect = QRect( 0, canvas->laneTop( rowIdx ) + 1, canvas->width(),
                      canvas->laneHeight( rowIdx ) - 2 );
    }

    const int span = rect.height() - 1;
    if( span < 1 ) return false;
    const int x0 = canvas->getXPosOfOffset( time + base );
    const int x1 = canvas->getXPosOfOffset( toTime + base );
    const int y0 = rect.bottom() - (int) ( scale.toNorm( value ) * span + 0.5 );
    const int y1 = rect.bottom() - (int) ( scale.toNorm( toValue ) * span + 0.5 );
    if( x0 < 0 || x1 < 0 ) return false;

    // The canvas auto-scrolls when the pointer leaves it, which would move the
    // time axis mid-gesture. The window is never shown in a test run, so grow
    // the canvas until both ends are inside it (drag-clip-edge does the same).
    const int needed = qMax( x0, x1 ) + 64;
    const int needH = rect.bottom() + 64;
    if( canvas->width() < needed || canvas->height() < needH )
        canvas->resize( qMax( canvas->width(), needed ),
                        qMax( canvas->height(), needH ) );

    auto send = [&]( QEvent::Type type, int x, int y, Qt::MouseButton button,
                     Qt::MouseButtons buttons ) {
        const QPointF local( x, y );
        QMouseEvent ev( type, local, canvas->mapToGlobal( local ), button,
                        buttons, mods );
        QApplication::sendEvent( canvas, &ev );
    };
    send( QEvent::MouseButtonPress,   x0, y0, Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseMove,          x1, y1, Qt::NoButton,   Qt::LeftButton );
    send( QEvent::MouseButtonRelease, x1, y1, Qt::LeftButton, Qt::NoButton );
    return true;
}

// ===========================================================================
// SStdMixerView members that are AUTOMATION code
// ===========================================================================
//
// They are defined here rather than in sstdmixerview.cpp on purpose: that file
// is already the largest in the app (timeline CONTRACT, known debt) and P6's
// budget is 100 lines of it. A member function may be defined in any
// translation unit of its class' library, and these belong with the rest of
// the automation UI.

SAutomationLaneUi &SStdMixerView::automationUi()
{
    if( !autoUi_ ) autoUi_ = new SAutomationLaneUi( *this );
    return *autoUi_;
}

// ONE pruning walk for EVERY per-track UI-state set (proposal 30 section E.5).
// The sets are keyed by STrack*, and a removed track leaves a dangling key that
// a later track allocated at the same address would inherit - a new lane that
// mysteriously remembers a deleted one's state. Walking the model once and
// pruning all of them together is what keeps them from drifting apart.
//
// The FOLD set used to be pruned here too. Fix/track-list-polish (m) moved
// fold state onto STrack itself (STrack::isCollapsed()) so it can be saved
// with the project; being an ordinary object attribute, it dies with the
// object automatically and needs no pruning walk of its own.
void SStdMixerView::pruneUiState()
{
    QSet<const STrack *> live;
    collectTracks( model_, live );

    for( auto it = takesExpanded_.begin(); it != takesExpanded_.end(); )
        if( live.contains( *it ) ) ++it; else it = takesExpanded_.erase( it );
    for( auto it = trackScale_.begin(); it != trackScale_.end(); )
        if( live.contains( it.key() ) ) ++it; else it = trackScale_.erase( it );
    if( autoUi_ ) autoUi_->pruneTo( live );
}

// The automation sub-lanes of `tk`, appended under its take lanes. Called by
// appendRowsFor, which owns the ORDER (track lane, take lanes, automation
// lanes) - every one of them a sub-lane, so the track's single head spans the
// whole group (laneGroupHeight) and assert-lane-alignment covers them for free.
void SStdMixerView::appendAutomationRowsFor( STrack *tk, SLink *lk,
                                             SObject *container, int depth )
{
    if( !autoUi_ ) return;
    const QVector<SAutoLaneRef> lanes = autoUi_->shownLanes( tk );
    for( const SAutoLaneRef &r : lanes ) {
        STrackRow row{ tk, lk, container, depth, false, false, -1 };
        row.subKind       = SubLaneKind::Automation;
        row.autoTarget    = r.target;
        row.autoSlotIndex = r.slotIndex;
        rows_.append( row );
    }
}

bool SStdMixerView::showAutomationLane( STrack *t, const QString &target,
                                        int slotIndex, bool show )
{
    if( !t || target.isEmpty() ) return false;
    SAutoLaneRef ref; ref.target = target; ref.slotIndex = slotIndex;
    SAutomationLaneUi &ui = automationUi();
    if( ui.isLaneShown( t, ref ) == show ) return true;
    ui.toggleLane( t, ref );
    return true;
}

void SStdMixerView::setClipEnvelopeEdit( bool on )
{
    automationUi().setEnvelopeEditEnabled( on );
    if( qContent_ ) qContent_->update();
}

bool SStdMixerView::dragAutomationPoint( const QString &owner, const QString &target,
                                         int slotIndex, int take, offset_t time,
                                         double value, offset_t toTime,
                                         double toValue,
                                         Qt::KeyboardModifiers mods )
{
    return automationUi().tkDrag( strackpath::parseQualified( owner ).idx, target,
                                  slotIndex, take, time, value, toTime, toValue,
                                  mods );
}

// The automation half of assert-lane-alignment (proposal 37 P6).
//
// Automation rows flow through the head/lane identity check for free — they
// are sub-lanes by the same rule take lanes are — so what is left to check is
// what only an automation row can get wrong: a row that still names a lane the
// model cannot resolve, and the contiguity every sub-lane group depends on
// (laneGroupHeight walks FORWARD from a track lane and stops at the first row
// that is not one of that track's sub-lanes, so an automation row separated
// from its track by another track's row would be silently uncovered by any
// head at all).
QString SStdMixerView::checkAutomationRows() const
{
    SProject *proj = SApplication::app().getCurrentProject();
    for( int i = 0; i < rows_.size(); ++i ) {
        const STrackRow &r = rows_.at( i );
        if( r.subKind != SubLaneKind::Automation ) continue;

        if( i == 0 || rows_.at( i - 1 ).track != r.track )
            return QString( "row %1: automation lane '%2' does not hang off its "
                            "track's lane group" ).arg( i ).arg( r.autoTarget );
        if( r.autoTarget.isEmpty() )
            return QString( "row %1: automation lane with no target" ).arg( i );

        const QList<int> path = strackpath::pathOf( model_, r.track );
        sautomation::OwnerRef o = sautomation::resolveOwner( proj, sautomation::rootNameOf( proj, model_ ), path, r.autoTarget, r.autoSlotIndex, -1 );
        if( !o.valid() )
            return QString( "row %1: automation lane '%2' (slot %3) on track "
                            "'%4' resolves to no owner" )
                       .arg( i ).arg( r.autoTarget ).arg( r.autoSlotIndex )
                       .arg( strackpath::pathToString( path ) );
        // NOT checked: that the model already owns the lane. Showing a lane is
        // VIEW state and deliberately creates nothing - an empty lane is drawn
        // as its default value, and the first gesture on it is what brings the
        // model lane into existence.
    }
    return QString();
}
