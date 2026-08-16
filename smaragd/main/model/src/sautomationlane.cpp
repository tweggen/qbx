#include "app/model/sautomationlane.h"

#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>

// --------------------------------------------------------------------- modes

QString sAutomationModeToString( SAutomationMode m )
{
    switch( m ) {
    case SAutomationMode::Off:   return QStringLiteral( "off" );
    case SAutomationMode::Trim:  return QStringLiteral( "trim" );
    case SAutomationMode::Read:  return QStringLiteral( "read" );
    case SAutomationMode::Touch: return QStringLiteral( "touch" );
    case SAutomationMode::Latch: return QStringLiteral( "latch" );
    case SAutomationMode::Write: return QStringLiteral( "write" );
    }
    return QStringLiteral( "trim" );
}

SAutomationMode sAutomationModeFromString( const QString &s, bool *ok )
{
    if( ok ) *ok = true;
    const QString l = s.trimmed().toLower();
    if( l == QLatin1String( "off" ) )   return SAutomationMode::Off;
    if( l == QLatin1String( "trim" ) )  return SAutomationMode::Trim;
    if( l == QLatin1String( "read" ) )  return SAutomationMode::Read;
    if( l == QLatin1String( "touch" ) ) return SAutomationMode::Touch;
    if( l == QLatin1String( "latch" ) ) return SAutomationMode::Latch;
    if( l == QLatin1String( "write" ) ) return SAutomationMode::Write;
    if( ok ) *ok = false;
    return SAutomationMode::Trim;   // design §11 decision 3
}

// ------------------------------------------------------------------ ParamRef

SParamRef SParamRef::parse( const QString &target )
{
    SParamRef r;
    const int colon = target.indexOf( QLatin1Char( ':' ) );
    if( colon <= 0 ) return r;

    const QString space = target.left( colon ).trimmed().toLower();
    const QString rest  = target.mid( colon + 1 ).trimmed();
    if( rest.isEmpty() ) return r;

    if( space == QLatin1String( "self" ) ) {
        // Q_PROPERTY names, capitalised exactly as SObject declares them; the
        // spelling is normalised so `self:volume` and `self:Volume` are ONE
        // lane rather than two that fight over the same fader.
        if( rest.compare( QLatin1String( "volume" ), Qt::CaseInsensitive ) == 0 ) {
            r.space = Space::Self; r.prop = QStringLiteral( "Volume" );
        } else if( rest.compare( QLatin1String( "muted" ), Qt::CaseInsensitive ) == 0 ) {
            r.space = Space::Self; r.prop = QStringLiteral( "Muted" );
        }
        // Pan is deliberately absent: the sink is mono until proposal 36 B5,
        // so a pan lane would store a number nothing could ever hear.
        return r;
    }
    if( space == QLatin1String( "param" ) ) {
        bool ok = false;
        const unsigned id = rest.toUInt( &ok );
        if( !ok ) return r;
        r.space   = Space::Param;
        r.paramId = (std::uint32_t) id;
        return r;
    }
    if( space == QLatin1String( "cut" ) ) {
        static const char *const kProps[] = { "Gain", "VelocityScale", "Transpose" };
        for( const char *p : kProps ) {
            if( rest.compare( QLatin1String( p ), Qt::CaseInsensitive ) == 0 ) {
                r.space = Space::Cut;
                r.prop  = QLatin1String( p );
                return r;
            }
        }
        return r;
    }
    return r;
}

QString SParamRef::toString() const
{
    switch( space ) {
    case Space::Self:  return QStringLiteral( "self:" ) + prop;
    case Space::Param: return QStringLiteral( "param:%1" ).arg( paramId );
    case Space::Cut:   return QStringLiteral( "cut:" ) + prop;
    case Space::Unknown: break;
    }
    return QString();
}

double SParamRef::defaultValue() const
{
    switch( space ) {
    case Space::Self:
        // dB for Volume (0 = unity, and the identity of Trim's SUM); 0 = audible
        // for Muted.
        return 0.0;
    case Space::Cut:
        if( prop == QLatin1String( "Gain" ) )          return 1.0;
        if( prop == QLatin1String( "VelocityScale" ) ) return 1.0;
        return 0.0;   // Transpose
    case Space::Param:
    case Space::Unknown:
        break;
    }
    return 0.0;
}

// ---------------------------------------------------------------------- lane

SAutomationLane::SAutomationLane( const QString &target )
    : targetSpelling_( SParamRef::parse( target ).isValid()
                           ? SParamRef::parse( target ).toString()
                           : target )
    , ref_( SParamRef::parse( target ) )
{
}

SAutomationLane::~SAutomationLane() = default;

bool SAutomationLane::setMode( SAutomationMode m )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( mode_ == m ) return false;
    mode_ = m;
    rebuildSnapshot_nolock();
    return true;
}

std::vector<SAutomationPoint> SAutomationLane::points() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return points_;
}

int SAutomationLane::pointCount() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return (int) points_.size();
}

bool SAutomationLane::setPoints( std::vector<SAutomationPoint> pts )
{
    std::sort( pts.begin(), pts.end(),
               []( const SAutomationPoint &a, const SAutomationPoint &b ) {
                   return a.frame < b.frame;
               } );
    // Two points on ONE frame cannot both be honoured by a breakpoint curve
    // (valueAt would have to be two-valued), so the LAST one wins — which is
    // what a drag that lands a point on top of another means.
    pts.erase( std::unique( pts.begin(), pts.end(),
                            []( const SAutomationPoint &a, const SAutomationPoint &b ) {
                                return a.frame == b.frame;
                            } ),
               pts.end() );

    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if( points_ == pts ) return false;
        points_ = std::move( pts );
        rebuildSnapshot_nolock();
    }
    return true;
}

// Caller holds mutex_. The snapshot is rebuilt WHOLE and swapped: a component
// already rendering a page keeps the pointer it read and finishes on a
// consistent table (THREADING rule 2).
void SAutomationLane::rebuildSnapshot_nolock()
{
    if( points_.empty() || mode_ == SAutomationMode::Off ) {
        snapshot_.reset();
        return;
    }

    std::vector<twCurvePoint> cps;
    cps.reserve( points_.size() + 1 );

    const bool isMute = ( ref_.space == SParamRef::Space::Self
                          && ref_.prop == QLatin1String( "Muted" ) );

    // THE ONE ASYMMETRY, and it is deliberate (see the header): a mute lane
    // holds AUDIBLE before its first point. twAutomationCurve holds the first
    // point's value there, so the anchor is made explicit rather than special
    // cased in the consumer — which keeps valueAt() and the engine agreeing.
    if( isMute && points_.front().frame > 0 ) {
        twCurvePoint z;
        z.frame = 0;
        z.value = 0.0;
        z.shape = twCurveShape::Step;
        cps.push_back( z );
    }

    for( const SAutomationPoint &p : points_ ) {
        twCurvePoint c;
        c.frame   = p.frame;
        c.value   = p.value;
        // A boolean is a boolean: an interpolated mute would be a fade the
        // gain stage's own 1.5 ms ramp already owns.
        c.shape   = isMute ? twCurveShape::Step : p.shape;
        c.tension = p.tension;
        cps.push_back( c );
    }

    snapshot_ = std::make_shared<const twAutomationCurve>( std::move( cps ),
                                                          ref_.defaultValue() );
}

double SAutomationLane::valueAt( offset_t frame ) const
{
    std::shared_ptr<const twAutomationCurve> snap;
    double dflt;
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        // Off must still REPORT what it stores — the mode decides whether the
        // engine consumes the lane, not whether the model knows its own values.
        if( points_.empty() ) return ref_.defaultValue();
        if( mode_ == SAutomationMode::Off ) {
            // Build the answer straight from the table rather than the (absent)
            // snapshot.
            const SAutomationPoint *lo = nullptr;
            for( const SAutomationPoint &p : points_ ) {
                if( p.frame <= frame ) lo = &p; else break;
            }
            return lo ? lo->value : points_.front().value;
        }
        snap  = snapshot_;
        dflt  = ref_.defaultValue();
    }
    return snap ? snap->valueAt( frame ) : dflt;
}

std::shared_ptr<const twAutomationCurve> SAutomationLane::snapshot() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return snapshot_;
}

// ------------------------------------------------------------------- ranges

void SAutomationLane::Range::unite( const Range &o )
{
    if( o.empty() ) return;
    if( empty() ) { *this = o; return; }
    if( o.start < start ) start = o.start;
    if( o.end   > end )   end   = o.end;
}

SAutomationLane::Range SAutomationLane::rangeAround( offset_t frame ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );

    Range r;
    r.start = frame;
    r.end   = std::numeric_limits<offset_t>::max();
    if( points_.empty() ) return r;

    // The point at or below `frame`, and the one after it.
    const SAutomationPoint *prev = nullptr;
    const SAutomationPoint *next = nullptr;
    for( const SAutomationPoint &p : points_ ) {
        if( p.frame < frame )      prev = &p;
        else if( p.frame > frame ) { next = &p; break; }
    }

    // A Step segment's value is its LEFT point's alone, so changing the point at
    // `frame` cannot move anything before it. An interpolating segment can.
    if( prev && prev->shape != twCurveShape::Step ) r.start = prev->frame;
    if( next ) r.end = next->frame;
    // A point before the first one moves the whole head of the curve (the
    // curve holds the first value there).
    if( frame < points_.front().frame ) r.start = std::min( r.start, (offset_t) 0 );
    return r;
}

SAutomationLane::Range SAutomationLane::fullRange() const
{
    Range r;
    r.start = 0;
    r.end   = std::numeric_limits<offset_t>::max();
    return r;
}

// ------------------------------------------------------------- serialization

QString SAutomationLane::shapeToString( twCurveShape c )
{
    switch( c ) {
    case twCurveShape::Step:   return QStringLiteral( "step" );
    case twCurveShape::Linear: return QStringLiteral( "linear" );
    case twCurveShape::Exp:    return QStringLiteral( "exp" );
    }
    return QStringLiteral( "linear" );
}

twCurveShape SAutomationLane::shapeFromString( const QString &s )
{
    const QString l = s.trimmed().toLower();
    if( l == QLatin1String( "step" ) ) return twCurveShape::Step;
    if( l == QLatin1String( "exp" ) )  return twCurveShape::Exp;
    return twCurveShape::Linear;
}

void SAutomationLane::serialize( QTextStream &o ) const
{
    std::vector<SAutomationPoint> pts;
    SAutomationMode               mode;
    QString                       name;
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        pts  = points_;
        mode = mode_;
        name = paramName_;
    }

    o << "<lane target='" << targetSpelling_ << "'"
      << " mode='" << sAutomationModeToString( mode ) << "'";
    if( !name.isEmpty() ) o << " name='" << name << "'";
    o << ">\n";
    for( const SAutomationPoint &p : pts ) {
        o << "<p t='" << (long long) p.frame << "'"
          << " v='" << QString::number( p.value, 'g', 17 ) << "'"
          << " c='" << shapeToString( p.shape ) << "'";
        if( p.shape == twCurveShape::Exp && p.tension != 0.0 )
            o << " k='" << QString::number( p.tension, 'g', 17 ) << "'";
        o << "/>\n";
    }
    o << "</lane>\n";
}

bool SAutomationLane::readFrom( const QDomElement &laneEl )
{
    const SParamRef ref = SParamRef::parse( laneEl.attribute( "target" ) );
    if( !ref.isValid() ) return false;

    std::vector<SAutomationPoint> pts;
    for( QDomNode n = laneEl.firstChild(); !n.isNull(); n = n.nextSibling() ) {
        if( !n.isElement() ) continue;
        const QDomElement pe = n.toElement();
        if( pe.tagName() != QLatin1String( "p" ) ) continue;
        SAutomationPoint p;
        p.frame   = (offset_t) pe.attribute( "t", "0" ).toLongLong();
        p.value   = pe.attribute( "v", "0" ).toDouble();
        p.shape   = shapeFromString( pe.attribute( "c", "linear" ) );
        p.tension = pe.attribute( "k", "0" ).toDouble();
        pts.push_back( p );
    }

    {
        std::lock_guard<std::mutex> lock( mutex_ );
        ref_            = ref;
        targetSpelling_ = ref.toString();
        paramName_      = laneEl.attribute( "name" );
        mode_           = sAutomationModeFromString( laneEl.attribute( "mode", "trim" ) );
    }
    setPoints( std::move( pts ) );
    {
        // The target and the mode both feed the snapshot (the mute anchor, the
        // Off short-circuit), and setPoints() is a no-op when the table did not
        // move — so the rebuild cannot be left to it.
        std::lock_guard<std::mutex> lock( mutex_ );
        rebuildSnapshot_nolock();
    }
    return true;
}
