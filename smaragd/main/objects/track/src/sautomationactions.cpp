#include "app/objects/track/sautomationactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/model/sobject.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "tw/core/twfraction.h"
#include "tw/core/twlog.h"

#include <QDomElement>
#include <algorithm>
#include <limits>

using namespace strackpath;

namespace {

constexpr offset_t kEndOfTime = std::numeric_limits<offset_t>::max();

offset_t parseFrames( const QString &s, offset_t dflt )
{
    if( s.isEmpty() ) return dflt;
    return (offset_t) parseFractionOrDouble( s.toStdString() ).toDouble();
}

}  // namespace

// ---------------------------------------------------------------- resolution

QString sautomation::rootNameOf( SProject *project, SObject *root )
{
    if( !project || !root ) return QString();
    return project->arrangementNameOf( root );
}

sautomation::OwnerRef sautomation::resolveOwner(
    SProject *project, const QString &pathRoot_,
    const QList<int> &ownerPath, const QString &target,
    int slotIndex, int take )
{
    OwnerRef out;
    out.ref = SParamRef::parse( target );
    if( !out.ref.isValid() ) return out;

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if( !root ) { out.ref = SParamRef(); return out; }

    switch( out.ref.space ) {
    case SParamRef::Space::Self: {
        // A track: the fader and the audio mute both live on twGainStage, which
        // only a track has.
        SObject *lane = splacements::laneAt( root, ownerPath );
        out.owner = dynamic_cast<STrack *>( lane );
        break;
    }
    case SParamRef::Space::Param: {
        if( slotIndex < 0 ) break;
        STrack *track = dynamic_cast<STrack *>( splacements::laneAt( root, ownerPath ) );
        SPluginChain *chain = track ? track->getPluginChain() : nullptr;
        out.owner = chain ? chain->getSlotAt( slotIndex ) : nullptr;
        break;
    }
    case SParamRef::Space::Cut: {
        SLink *link = splacements::placementAt( root, ownerPath );
        if( !link ) break;
        SObject *obj = &link->getSObject();
        // A take stack answers for the addressed take, and for the ACTIVE one
        // when none is named - so an INACTIVE take keeps its own envelope
        // (design §3.3: the lane travels with the WINDOW).
        if( SClipWindow *w = obj->windowTakeAt( take ) ) obj = &w->asObject();
        out.owner = obj;
        break;
    }
    case SParamRef::Space::Unknown:
        break;
    }
    if( !out.owner ) out.ref = SParamRef();
    return out;
}

SAutomationLane::Range sautomation::editRange( const SAutomationLane &lane,
                                               const SParamRef &ref, offset_t frame )
{
    SAutomationLane::Range r = lane.rangeAround( frame );
    // A PLUGIN IS CLASS 1 (design F9): its DSP state at any position depends on
    // everything before it, so an edit at `a` can change every page after `a`
    // and the range cannot be closed on the right. A gain stage is class
    // infinity and pure, so its range stays exact — which is the whole reason
    // the two are distinguished here rather than both being made safe.
    if( ref.space == SParamRef::Space::Param ) r.end = kEndOfTime;
    return r;
}

void sautomation::commit( OwnerRef &o, SAutomationLane &lane,
                          const SAutomationLane::Range &r )
{
    if( !o.owner ) return;
    o.owner->onAutomationChanged( lane, r.start, r.end );
    lane.notifyChanged( r.start, r.end );
}

void sautomation::writePointChildren( QDomElement &elem,
                                      const std::vector<SAutomationPoint> &pts )
{
    QDomDocument doc = elem.ownerDocument();
    for( const SAutomationPoint &p : pts ) {
        QDomElement pe = doc.createElement( QStringLiteral( "p" ) );
        pe.setAttribute( "t", QString::number( (qlonglong) p.frame ) );
        pe.setAttribute( "v", QString::number( p.value, 'g', 17 ) );
        pe.setAttribute( "c", SAutomationLane::shapeToString( p.shape ) );
        if( p.shape == twCurveShape::Exp && p.tension != 0.0 )
            pe.setAttribute( "k", QString::number( p.tension, 'g', 17 ) );
        elem.appendChild( pe );
    }
}

std::vector<SAutomationPoint> sautomation::readPointChildren( const QDomElement &elem )
{
    std::vector<SAutomationPoint> out;
    for( QDomNode n = elem.firstChild(); !n.isNull(); n = n.nextSibling() ) {
        if( !n.isElement() ) continue;
        const QDomElement pe = n.toElement();
        if( pe.tagName() != QLatin1String( "p" ) ) continue;
        SAutomationPoint p;
        p.frame   = parseFrames( pe.attribute( "t", "0" ), 0 );
        p.value   = pe.attribute( "v", "0" ).toDouble();
        p.shape   = SAutomationLane::shapeFromString( pe.attribute( "c", "linear" ) );
        p.tension = pe.attribute( "k", "0" ).toDouble();
        out.push_back( p );
    }
    return out;
}

SAutomationLane *sautomation::readLaneFor( SObject *track, const QString &target )
{
    if( !track ) return nullptr;
    SAutomationLane *lane = track->automationLane( target );
    if( !lane ) return nullptr;
    switch( lane->mode() ) {
    case SAutomationMode::Read:
    case SAutomationMode::Touch:
    case SAutomationMode::Latch:
    case SAutomationMode::Write:
        return lane;
    case SAutomationMode::Off:
    case SAutomationMode::Trim:
        // The static value is still the thing the user is editing.
        return nullptr;
    }
    return nullptr;
}

SAction *sautomation::pointAtLocatorAction( const QList<int> &ownerPath,
                                            const QString &target,
                                            offset_t frame, double value )
{
    std::vector<SAutomationPoint> pts( 1 );
    pts[0].frame = frame;
    pts[0].value = value;
    pts[0].shape = ( target == QLatin1String( "self:Muted" ) )
                       ? twCurveShape::Step : twCurveShape::Linear;
    // A ONE-FRAME window: the gesture writes a point where the playhead is and
    // leaves the rest of the lane alone.
    return new SSetAutomationPointsAction( ownerPath, target, frame, frame + 1,
                                           std::move( pts ), -1, -1 );
}

// ---------------------------------------------------------- shared XML shape

namespace {

void writeCommon( QDomElement &elem, const QList<int> &ownerPath,
                  const QString &target, int slotIndex, int take )
{
    elem.setAttribute( "owner", pathToString( ownerPath ) );
    elem.setAttribute( "target", target );
    if( slotIndex >= 0 ) elem.setAttribute( "slotIndex", slotIndex );
    if( take >= 0 )      elem.setAttribute( "take", take );
}

void readCommon( const QDomElement &elem, QString &pathRoot_,
                 QList<int> &ownerPath,
                 QString &target, int &slotIndex, int &take )
{
    ownerPath = parseInto( pathRoot_, elem.attribute( "owner" ) );
    target    = elem.attribute( "target" );
    slotIndex = elem.attribute( "slotIndex", "-1" ).toInt();
    take      = elem.attribute( "take", "-1" ).toInt();
}

const char *const kCommonAttrs[] = { "owner", "target", "slotIndex", "take" };

QStringList commonAttrs()
{
    QStringList l;
    for( const char *a : kCommonAttrs ) l << QLatin1String( a );
    return l;
}

}  // namespace

// ============================================================ add / remove lane

SAddAutomationLaneAction::SAddAutomationLaneAction(
    const QList<int> &ownerPath, const QString &target, SAutomationMode mode,
    int slotIndex, int take, std::vector<SAutomationPoint> pts,
    const QString &paramName )
    : ownerPath_( ownerPath ), target_( target ), paramName_( paramName )
    , mode_( mode ), slotIndex_( slotIndex ), take_( take )
    , points_( std::move( pts ) ), hasPoints_( true )
{
}

SApplyResult SAddAutomationLaneAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) return { false, nullptr };

    SAutomationLane *existing = o.owner->automationLane( target_ );
    const bool existed = ( existing != nullptr );

    SAutomationMode               oldMode = SAutomationMode::Trim;
    std::vector<SAutomationPoint> oldPts;
    if( existed ) {
        oldMode = existing->mode();
        oldPts  = existing->points();
    }

    SAutomationLane *lane = o.owner->ensureAutomationLane( target_ );
    if( !lane ) return { false, nullptr };
    if( !paramName_.isEmpty() ) lane->setParamName( paramName_ );
    lane->setMode( mode_ );
    if( hasPoints_ ) lane->setPoints( points_ );

    // A lane appearing (or being replaced wholesale) can change ANY position it
    // covers, so the range is the whole of it.
    sautomation::commit( o, *lane, lane->fullRange() );

    SAction *inverse = existed
        ? (SAction *) new SAddAutomationLaneAction( ownerPath_, target_, oldMode,
                                                    slotIndex_, take_,
                                                    std::move( oldPts ), paramName_ )
        : (SAction *) new SRemoveAutomationLaneAction( ownerPath_, target_,
                                                       slotIndex_, take_ );
    return { true, inverse };
}

void SAddAutomationLaneAction::writeXml( QDomElement &elem ) const
{
    writeCommon( elem, ownerPath_, target_, slotIndex_, take_ );
    elem.setAttribute( "mode", sAutomationModeToString( mode_ ) );
    if( !paramName_.isEmpty() ) elem.setAttribute( "name", paramName_ );
    if( hasPoints_ ) sautomation::writePointChildren( elem, points_ );
}

bool SAddAutomationLaneAction::readXml( const QDomElement &elem, int )
{
    readCommon( elem, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    mode_      = sAutomationModeFromString( elem.attribute( "mode", "trim" ) );
    paramName_ = elem.attribute( "name" );
    points_    = sautomation::readPointChildren( elem );
    hasPoints_ = !points_.empty();
    return true;
}

QStringList SAddAutomationLaneAction::knownAttributes() const
{
    return commonAttrs() << QStringLiteral( "mode" ) << QStringLiteral( "name" );
}

SRemoveAutomationLaneAction::SRemoveAutomationLaneAction(
    const QList<int> &ownerPath, const QString &target, int slotIndex, int take )
    : ownerPath_( ownerPath ), target_( target )
    , slotIndex_( slotIndex ), take_( take )
{
}

SApplyResult SRemoveAutomationLaneAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) return { false, nullptr };

    SAutomationLane *lane = o.owner->automationLane( target_ );
    if( !lane ) return { false, nullptr };

    // THE INVERSE CARRIES THE WHOLE POINT LIST (design §3.4) — captured before
    // the lane is destroyed, because afterwards there is nothing to ask.
    const SAutomationMode               mode = lane->mode();
    std::vector<SAutomationPoint>       pts  = lane->points();
    const QString                       nm   = lane->paramName();
    const SAutomationLane::Range        r    = lane->fullRange();

    // Empty the lane BEFORE dropping it, so the commit below hands the owner a
    // null snapshot and the engine is left on the scalar path. Dropping it
    // first would leave the old curve in the gain stage for ever.
    lane->setPoints( {} );
    lane->setMode( SAutomationMode::Off );
    sautomation::commit( o, *lane, r );
    o.owner->removeAutomationLane( target_ );
    o.owner->applyAutomationToEngine();

    return { true, new SAddAutomationLaneAction( ownerPath_, target_, mode,
                                                 slotIndex_, take_,
                                                 std::move( pts ), nm ) };
}

void SRemoveAutomationLaneAction::writeXml( QDomElement &elem ) const
{
    writeCommon( elem, ownerPath_, target_, slotIndex_, take_ );
}

bool SRemoveAutomationLaneAction::readXml( const QDomElement &elem, int )
{
    readCommon( elem, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    return true;
}

QStringList SRemoveAutomationLaneAction::knownAttributes() const
{
    return commonAttrs();
}

// ==================================================================== the mode

SSetAutomationModeAction::SSetAutomationModeAction(
    const QList<int> &ownerPath, const QString &target, SAutomationMode mode,
    int slotIndex, int take )
    : ownerPath_( ownerPath ), target_( target ), mode_( mode )
    , slotIndex_( slotIndex ), take_( take )
{
}

SApplyResult SSetAutomationModeAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) return { false, nullptr };

    SAutomationLane *lane = o.owner->automationLane( target_ );
    if( !lane ) return { false, nullptr };

    const SAutomationMode old = lane->mode();
    if( old != mode_ ) {
        lane->setMode( mode_ );
        // Off <-> anything changes whether the curve is consumed AT ALL, and
        // Trim <-> Read changes whether the fader is summed in, so every mode
        // change moves audio everywhere the lane reaches.
        sautomation::commit( o, *lane, lane->fullRange() );
    }
    return { true, new SSetAutomationModeAction( ownerPath_, target_, old,
                                                 slotIndex_, take_ ) };
}

void SSetAutomationModeAction::writeXml( QDomElement &elem ) const
{
    writeCommon( elem, ownerPath_, target_, slotIndex_, take_ );
    elem.setAttribute( "mode", sAutomationModeToString( mode_ ) );
}

bool SSetAutomationModeAction::readXml( const QDomElement &elem, int )
{
    readCommon( elem, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    mode_ = sAutomationModeFromString( elem.attribute( "mode", "trim" ) );
    return true;
}

QStringList SSetAutomationModeAction::knownAttributes() const
{
    return commonAttrs() << QStringLiteral( "mode" );
}

// ================================================================ single points

SAddAutomationPointAction::SAddAutomationPointAction(
    const QList<int> &ownerPath, const QString &target, offset_t time, double value,
    twCurveShape shape, double tension, int slotIndex, int take )
    : ownerPath_( ownerPath ), target_( target ), time_( time ), value_( value )
    , shape_( shape ), tension_( tension ), slotIndex_( slotIndex ), take_( take )
{
}

SApplyResult SAddAutomationPointAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) return { false, nullptr };

    SAutomationLane *lane = o.owner->ensureAutomationLane( target_ );
    if( !lane ) return { false, nullptr };

    std::vector<SAutomationPoint> pts = lane->points();
    // An existing point on the frame is REPLACED — absolute, like every verb
    // here — and the inverse then has to put the old one back rather than
    // remove ours.
    const SAutomationPoint *old = nullptr;
    for( const SAutomationPoint &p : pts ) if( p.frame == time_ ) { old = &p; break; }
    const bool          replaced = ( old != nullptr );
    SAutomationPoint    oldPoint;
    if( replaced ) oldPoint = *old;

    SAutomationPoint np;
    np.frame = time_; np.value = value_; np.shape = shape_; np.tension = tension_;
    pts.push_back( np );

    SAutomationLane::Range r = sautomation::editRange( *lane, o.ref, time_ );
    lane->setPoints( std::move( pts ) );
    r.unite( sautomation::editRange( *lane, o.ref, time_ ) );
    sautomation::commit( o, *lane, r );

    SAction *inverse = replaced
        ? (SAction *) new SAddAutomationPointAction( ownerPath_, target_,
                                                     oldPoint.frame, oldPoint.value,
                                                     oldPoint.shape, oldPoint.tension,
                                                     slotIndex_, take_ )
        : (SAction *) new SRemoveAutomationPointAction( ownerPath_, target_,
                                                        time_, value_,
                                                        slotIndex_, take_ );
    return { true, inverse };
}

void SAddAutomationPointAction::writeXml( QDomElement &elem ) const
{
    writeCommon( elem, ownerPath_, target_, slotIndex_, take_ );
    elem.setAttribute( "time", QString::number( (qlonglong) time_ ) );
    elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
    elem.setAttribute( "curve", SAutomationLane::shapeToString( shape_ ) );
    if( tension_ != 0.0 )
        elem.setAttribute( "tension", QString::number( tension_, 'g', 17 ) );
}

bool SAddAutomationPointAction::readXml( const QDomElement &elem, int )
{
    readCommon( elem, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    time_    = parseFrames( elem.attribute( "time", "0" ), 0 );
    value_   = elem.attribute( "value", "0" ).toDouble();
    shape_   = SAutomationLane::shapeFromString( elem.attribute( "curve", "linear" ) );
    tension_ = elem.attribute( "tension", "0" ).toDouble();
    return true;
}

QStringList SAddAutomationPointAction::knownAttributes() const
{
    return commonAttrs() << QStringLiteral( "time" ) << QStringLiteral( "value" )
                         << QStringLiteral( "curve" ) << QStringLiteral( "tension" );
}

SRemoveAutomationPointAction::SRemoveAutomationPointAction(
    const QList<int> &ownerPath, const QString &target, offset_t time, double value,
    int slotIndex, int take )
    : ownerPath_( ownerPath ), target_( target ), time_( time ), value_( value )
    , matchValue_( true ), slotIndex_( slotIndex ), take_( take )
{
}

SApplyResult SRemoveAutomationPointAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) return { false, nullptr };

    SAutomationLane *lane = o.owner->automationLane( target_ );
    if( !lane ) return { false, nullptr };

    std::vector<SAutomationPoint> pts = lane->points();
    std::size_t hit = pts.size();
    for( std::size_t i = 0; i < pts.size(); ++i ) {
        if( pts[i].frame != time_ ) continue;
        // Addressed by the OLD (time, value) pair (design §3.4). A script that
        // gives only the time still resolves, because a lane holds at most one
        // point per frame.
        if( matchValue_ && pts[i].value != value_ ) continue;
        hit = i;
        break;
    }
    if( hit == pts.size() ) return { false, nullptr };

    const SAutomationPoint gone = pts[hit];
    SAutomationLane::Range r = sautomation::editRange( *lane, o.ref, time_ );
    pts.erase( pts.begin() + (long) hit );
    lane->setPoints( std::move( pts ) );
    r.unite( sautomation::editRange( *lane, o.ref, time_ ) );
    sautomation::commit( o, *lane, r );

    return { true, new SAddAutomationPointAction( ownerPath_, target_, gone.frame,
                                                  gone.value, gone.shape, gone.tension,
                                                  slotIndex_, take_ ) };
}

void SRemoveAutomationPointAction::writeXml( QDomElement &elem ) const
{
    writeCommon( elem, ownerPath_, target_, slotIndex_, take_ );
    elem.setAttribute( "time", QString::number( (qlonglong) time_ ) );
    if( matchValue_ )
        elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
}

bool SRemoveAutomationPointAction::readXml( const QDomElement &elem, int )
{
    readCommon( elem, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    time_       = parseFrames( elem.attribute( "time", "0" ), 0 );
    matchValue_ = elem.hasAttribute( "value" );
    value_      = elem.attribute( "value", "0" ).toDouble();
    return true;
}

QStringList SRemoveAutomationPointAction::knownAttributes() const
{
    return commonAttrs() << QStringLiteral( "time" ) << QStringLiteral( "value" );
}

SMoveAutomationPointAction::SMoveAutomationPointAction(
    const QList<int> &ownerPath, const QString &target, offset_t time, double value,
    offset_t toTime, double toValue, int slotIndex, int take )
    : ownerPath_( ownerPath ), target_( target ), time_( time ), value_( value )
    , matchValue_( true ), toTime_( toTime ), toValue_( toValue )
    , slotIndex_( slotIndex ), take_( take )
{
}

SApplyResult SMoveAutomationPointAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) return { false, nullptr };

    SAutomationLane *lane = o.owner->automationLane( target_ );
    if( !lane ) return { false, nullptr };

    std::vector<SAutomationPoint> pts = lane->points();
    std::size_t hit = pts.size();
    for( std::size_t i = 0; i < pts.size(); ++i ) {
        if( pts[i].frame != time_ ) continue;
        if( matchValue_ && pts[i].value != value_ ) continue;
        hit = i;
        break;
    }
    if( hit == pts.size() ) return { false, nullptr };

    const SAutomationPoint from = pts[hit];

    // BOTH ENDS of the move are dirty: the segments around where it WAS and the
    // segments around where it LANDS. Taking the union is what lets a case
    // assert byte identity either side of an edit (P5 AC5).
    SAutomationLane::Range r = sautomation::editRange( *lane, o.ref, time_ );
    r.unite( sautomation::editRange( *lane, o.ref, toTime_ ) );

    pts[hit].frame = toTime_;
    pts[hit].value = toValue_;
    lane->setPoints( std::move( pts ) );

    r.unite( sautomation::editRange( *lane, o.ref, toTime_ ) );
    r.unite( sautomation::editRange( *lane, o.ref, time_ ) );
    sautomation::commit( o, *lane, r );

    return { true, new SMoveAutomationPointAction( ownerPath_, target_,
                                                   toTime_, toValue_,
                                                   from.frame, from.value,
                                                   slotIndex_, take_ ) };
}

void SMoveAutomationPointAction::writeXml( QDomElement &elem ) const
{
    writeCommon( elem, ownerPath_, target_, slotIndex_, take_ );
    elem.setAttribute( "time", QString::number( (qlonglong) time_ ) );
    if( matchValue_ )
        elem.setAttribute( "value", QString::number( value_, 'g', 17 ) );
    elem.setAttribute( "toTime", QString::number( (qlonglong) toTime_ ) );
    elem.setAttribute( "toValue", QString::number( toValue_, 'g', 17 ) );
}

bool SMoveAutomationPointAction::readXml( const QDomElement &elem, int )
{
    readCommon( elem, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    time_       = parseFrames( elem.attribute( "time", "0" ), 0 );
    matchValue_ = elem.hasAttribute( "value" );
    value_      = elem.attribute( "value", "0" ).toDouble();
    toTime_     = parseFrames( elem.attribute( "toTime", "0" ), 0 );
    toValue_    = elem.attribute( "toValue", "0" ).toDouble();
    return true;
}

QStringList SMoveAutomationPointAction::knownAttributes() const
{
    return commonAttrs() << QStringLiteral( "time" ) << QStringLiteral( "value" )
                         << QStringLiteral( "toTime" ) << QStringLiteral( "toValue" );
}

QString SMoveAutomationPointAction::mergeKey() const
{
    // A DRAG of one point is one undo step. Keyed on where the point STARTED,
    // which is what the successive events of a drag share.
    return QStringLiteral( "move-automation-point:%1:%2:%3:%4" )
        .arg( qualifiedToString( pathRoot_, ownerPath_ ) ).arg( slotIndex_ )
        .arg( target_ ).arg( (qlonglong) time_ );
}

bool SMoveAutomationPointAction::mergeWith( const SAction *later )
{
    const SMoveAutomationPointAction *o =
        dynamic_cast<const SMoveAutomationPointAction *>( later );
    if( !o ) return false;
    if( o->ownerPath_ != ownerPath_ || o->target_ != target_
        || o->slotIndex_ != slotIndex_ ) return false;
    // The newer action starts where we LEFT the point, or the two are not one
    // gesture.
    if( o->time_ != toTime_ ) return false;
    toTime_  = o->toTime_;
    toValue_ = o->toValue_;
    return true;
}

// ================================================================== set-points

SSetAutomationPointsAction::SSetAutomationPointsAction(
    const QList<int> &ownerPath, const QString &target, offset_t from, offset_t to,
    std::vector<SAutomationPoint> pts, int slotIndex, int take )
    : ownerPath_( ownerPath ), target_( target ), from_( from ), to_( to )
    , points_( std::move( pts ) ), slotIndex_( slotIndex ), take_( take )
{
}

SApplyResult SSetAutomationPointsAction::apply( SProject *project )
{
    sautomation::OwnerRef o =
        sautomation::resolveOwner( project, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    if( !o.valid() ) return { false, nullptr };

    SAutomationLane *lane = o.owner->ensureAutomationLane( target_ );
    if( !lane ) return { false, nullptr };

    const offset_t hi = ( to_ < 0 ) ? kEndOfTime : to_;

    // THE ABSOLUTE NEW STATE OF THE WINDOW [from, to): everything in it is
    // replaced, everything outside is untouched. The inverse is the same verb
    // over the same window carrying what was there.
    std::vector<SAutomationPoint> kept, replaced;
    for( const SAutomationPoint &p : lane->points() ) {
        if( p.frame >= from_ && p.frame < hi ) replaced.push_back( p );
        else                                   kept.push_back( p );
    }
    for( const SAutomationPoint &p : points_ ) {
        if( p.frame < from_ || p.frame >= hi ) continue;   // outside the window
        kept.push_back( p );
    }

    SAutomationLane::Range r;
    r.start = from_;
    r.end   = hi;
    // The segments that REACH INTO the window from either side move too.
    r.unite( sautomation::editRange( *lane, o.ref, from_ ) );
    if( hi != kEndOfTime ) r.unite( sautomation::editRange( *lane, o.ref, hi ) );

    lane->setPoints( std::move( kept ) );

    r.unite( sautomation::editRange( *lane, o.ref, from_ ) );
    if( hi != kEndOfTime ) r.unite( sautomation::editRange( *lane, o.ref, hi ) );
    if( o.ref.space == SParamRef::Space::Param ) r.end = kEndOfTime;
    sautomation::commit( o, *lane, r );

    return { true, new SSetAutomationPointsAction( ownerPath_, target_, from_, to_,
                                                   std::move( replaced ),
                                                   slotIndex_, take_ ) };
}

void SSetAutomationPointsAction::writeXml( QDomElement &elem ) const
{
    writeCommon( elem, ownerPath_, target_, slotIndex_, take_ );
    elem.setAttribute( "from", QString::number( (qlonglong) from_ ) );
    elem.setAttribute( "to", QString::number( (qlonglong) to_ ) );
    sautomation::writePointChildren( elem, points_ );
}

bool SSetAutomationPointsAction::readXml( const QDomElement &elem, int )
{
    readCommon( elem, pathRoot_, ownerPath_, target_, slotIndex_, take_ );
    from_   = parseFrames( elem.attribute( "from", "0" ), 0 );
    to_     = parseFrames( elem.attribute( "to", "-1" ), -1 );
    points_ = sautomation::readPointChildren( elem );
    return true;
}

QStringList SSetAutomationPointsAction::knownAttributes() const
{
    return commonAttrs() << QStringLiteral( "from" ) << QStringLiteral( "to" );
}

QString SSetAutomationPointsAction::mergeKey() const
{
    // OWNER + TARGET (design §3.4). Curve drawing and a Touch/Latch/Write pass
    // both issue a stream of these; without coalescing a single gesture would
    // become hundreds of undo steps.
    return QStringLiteral( "set-automation-points:%1:%2:%3" )
        .arg( qualifiedToString( pathRoot_, ownerPath_ ) ).arg( slotIndex_ ).arg( target_ );
}

bool SSetAutomationPointsAction::mergeWith( const SAction *later )
{
    const SSetAutomationPointsAction *o =
        dynamic_cast<const SSetAutomationPointsAction *>( later );
    if( !o ) return false;
    if( o->ownerPath_ != ownerPath_ || o->target_ != target_
        || o->slotIndex_ != slotIndex_ || o->take_ != take_ ) return false;
    // ONLY an identical window merges. A wider one would silently drop the
    // points this action wrote outside the newcomer's span, and the captured
    // inverse would then no longer describe the state we started from.
    if( o->from_ != from_ || o->to_ != to_ ) return false;
    points_ = o->points_;
    return true;
}

// ------------------------------------------------------------- registration

static const bool s_reg_addautolane = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-automation-lane" ),
        []{ return new SAddAutomationLaneAction; } ), true );

static const bool s_reg_removeautolane = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "remove-automation-lane" ),
        []{ return new SRemoveAutomationLaneAction; } ), true );

static const bool s_reg_setautomode = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-automation-mode" ),
        []{ return new SSetAutomationModeAction; } ), true );

static const bool s_reg_addautopoint = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-automation-point" ),
        []{ return new SAddAutomationPointAction; } ), true );

static const bool s_reg_removeautopoint = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "remove-automation-point" ),
        []{ return new SRemoveAutomationPointAction; } ), true );

static const bool s_reg_moveautopoint = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "move-automation-point" ),
        []{ return new SMoveAutomationPointAction; } ), true );

static const bool s_reg_setautopoints = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-automation-points" ),
        []{ return new SSetAutomationPointsAction; } ), true );
