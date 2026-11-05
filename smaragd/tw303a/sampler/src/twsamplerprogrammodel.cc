// twSamplerProgramModel: the editable NassauDigital program, a thin forwarder
// to the core's model, edits and XML (proposal 56 §4.4, Q1).

#include "tw/sampler/twsamplerprogrammodel.h"

#include "twsamplerimpl.h"

#include "nd_edit.h"
#include "nd_overlay.h"
#include "nd_program.h"
#include "nd_program_xml.h"
#include "nd_xml.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace audio {

struct twSamplerProgramModel::Impl {
    nd::Program program;
};

namespace {

void setError( std::string *error, const std::string &why )
{
    if( error ) *error = why;
}

template <typename T>
twSamplerParamKind kindOf( const nd::ParamDesc<T> *p )
{
    if( !p ) return twSamplerParamKind::Unknown;
    switch( p->kind ) {
    case nd::ParamKind::Int:
    case nd::ParamKind::Real:   return twSamplerParamKind::Numeric;
    case nd::ParamKind::Enum:   return twSamplerParamKind::Enum;
    case nd::ParamKind::Bool:   return twSamplerParamKind::Bool;
    case nd::ParamKind::String: return twSamplerParamKind::String;
    }
    return twSamplerParamKind::Unknown;
}

std::vector<nd::ParamEdit> toCore( const std::vector<twSamplerEdit> &edits )
{
    std::vector<nd::ParamEdit> out;
    out.reserve( edits.size() );
    for( const twSamplerEdit &e : edits )
        out.push_back( e.relative ? nd::ParamEdit::by( e.attr, e.delta )
                                  : nd::ParamEdit::absolute( e.attr, e.value ) );
    return out;
}

std::vector<nd::PriorValue> toCore( const std::vector<twSamplerPrior> &prior )
{
    std::vector<nd::PriorValue> out;
    out.reserve( prior.size() );
    for( const twSamplerPrior &p : prior ) out.push_back( { p.id, p.attr, p.value } );
    return out;
}

void fromCore( const std::vector<nd::PriorValue> &in, std::vector<twSamplerPrior> *out )
{
    if( !out ) return;
    out->clear();
    for( const nd::PriorValue &p : in ) out->push_back( { p.id, p.attr, p.value } );
}

// Fill `obj` from attribute text through the core's table. An attribute the
// table does not know is REFUSED here (a script typo), and so is `id` -- a
// ZONE's explicit id (QBX-265) is taken out by addZone() before this runs, so
// what reaches here is a layer's, which is always assigned.
template <typename T>
bool fillFromAttrs( const std::vector<nd::ParamDesc<T>> &table, const twSamplerAttrs &attrs,
                    T &obj, const char *what, std::string *error )
{
    for( const auto &a : attrs ) {
        if( a.first == "id" ) {
            setError( error, std::string( "a " ) + what +
                                 "'s id is assigned from nextId, never taken (DESIGN §2)" );
            return false;
        }
        const nd::ParamDesc<T> *p = nullptr;
        for( const auto &d : table )
            if( a.first == d.name ) p = &d;
        if( !p ) {
            setError( error, std::string( "unknown " ) + what + " attribute \"" + a.first + "\"" );
            return false;
        }
        std::string why;
        if( !nd::parseParam( *p, obj, a.second, why ) ) {
            setError( error, why );
            return false;
        }
    }
    return true;
}

// The zone `z` as the core writes it: programToXml of a program holding only
// it, so the text is exactly the core's grammar (extras included).
std::string zoneText( const nd::Zone &z )
{
    nd::Program tmp;
    tmp.zones.push_back( z );
    const nd::XmlNode root = nd::programToXml( tmp );
    return root.children.empty() ? std::string() : nd::writeXml( root.children.front() );
}

std::string layerText( const nd::Layer &l )
{
    nd::Zone z;
    z.id = 1;
    z.layers.push_back( l );
    nd::Program tmp;
    tmp.zones.push_back( z );
    const nd::XmlNode root = nd::programToXml( tmp );
    if( root.children.empty() || root.children.front().children.empty() ) return std::string();
    return nd::writeXml( root.children.front().children.front() );
}

// Read back what zoneText() wrote, through the core's own reader (which checks
// ids, values, the layer count and keeps unknown attributes).
bool parseZoneText( const std::string &xml, nd::Zone &out, std::string *error )
{
    const nd::XmlParseResult x = nd::parseXml( xml );
    if( !x.ok ) { setError( error, "XML: " + x.error ); return false; }
    if( x.root.name != "zone" ) { setError( error, "expected <zone>, got <" + x.root.name + ">" ); return false; }
    nd::XmlNode prog;
    prog.name = nd::kProgramElement;
    prog.children.push_back( x.root );
    nd::ProgramParseResult r = nd::programFromXml( prog );
    if( !r.ok ) { setError( error, r.error ); return false; }
    if( r.program.zones.size() != 1 ) { setError( error, "no zone in the text" ); return false; }
    out = std::move( r.program.zones.front() );
    return true;
}

bool parseLayerText( const std::string &xml, nd::Layer &out, std::string *error )
{
    const nd::XmlParseResult x = nd::parseXml( xml );
    if( !x.ok ) { setError( error, "XML: " + x.error ); return false; }
    if( x.root.name != "layer" ) { setError( error, "expected <layer>, got <" + x.root.name + ">" ); return false; }
    nd::XmlNode zone;
    zone.name = "zone";
    // An id the layer cannot carry (ids are positive and unique), so the
    // wrapper never collides with it.
    zone.attributes.emplace_back( "id", "4294967295" );
    zone.children.push_back( x.root );
    nd::XmlNode prog;
    prog.name = nd::kProgramElement;
    prog.children.push_back( zone );
    nd::ProgramParseResult r = nd::programFromXml( prog );
    if( !r.ok ) { setError( error, r.error ); return false; }
    if( r.program.zones.size() != 1 || r.program.zones.front().layers.size() != 1 ) {
        setError( error, "no layer in the text" );
        return false;
    }
    out = std::move( r.program.zones.front().layers.front() );
    return true;
}

bool idInUse( const nd::Program &p, std::uint32_t id )
{
    return p.findZone( id ) || p.findLayer( id ).second;
}

}  // namespace

// ------------------------------------------------------------ lifetime -------

twSamplerProgramModel::twSamplerProgramModel() : impl_( new Impl ) {}
twSamplerProgramModel::twSamplerProgramModel( const twSamplerProgramModel &o )
    : impl_( new Impl( *o.impl_ ) ) {}
twSamplerProgramModel &twSamplerProgramModel::operator=( const twSamplerProgramModel &o )
{
    if( this != &o ) *impl_ = *o.impl_;
    return *this;
}
twSamplerProgramModel::~twSamplerProgramModel() = default;

// ------------------------------------------------------------ grammar --------

bool twSamplerProgramModel::fromXml( const std::string &xml, twSamplerProgramModel &out,
                                     std::string *error )
{
    nd::ProgramParseResult r = nd::parseProgram( xml );
    if( !r.ok ) {
        setError( error, r.error );
        return false;
    }
    out.impl_->program = std::move( r.program );
    return true;
}

std::string twSamplerProgramModel::toXml() const
{
    return nd::writeProgram( impl_->program );
}

bool twSamplerProgramModel::operator==( const twSamplerProgramModel &o ) const
{
    return impl_->program == o.impl_->program;
}

bool twSamplerProgramModel::equalsIgnoringNextId( const twSamplerProgramModel &o ) const
{
    nd::Program a = impl_->program;
    a.nextId      = o.impl_->program.nextId;
    return a == o.impl_->program;
}

std::shared_ptr<const twSamplerProgram> twSamplerProgramModel::snapshot() const
{
    std::shared_ptr<twSamplerProgram> p( new twSamplerProgram );
    p->impl_->program = std::make_shared<const nd::Program>( impl_->program );
    p->serial_        = nextSamplerProgramSerial();
    return p;
}

// ------------------------------------------------------------ reading --------

std::uint32_t twSamplerProgramModel::nextId() const { return impl_->program.nextId; }
std::size_t   twSamplerProgramModel::zoneCount() const { return impl_->program.zones.size(); }

std::vector<std::uint32_t> twSamplerProgramModel::zoneIds() const
{
    std::vector<std::uint32_t> ids;
    for( const nd::Zone &z : impl_->program.zones ) ids.push_back( z.id );
    return ids;
}

std::vector<std::uint32_t> twSamplerProgramModel::layerIds( std::uint32_t zoneId ) const
{
    std::vector<std::uint32_t> ids;
    if( const nd::Zone *z = impl_->program.findZone( zoneId ) )
        for( const nd::Layer &l : z->layers ) ids.push_back( l.id );
    return ids;
}

bool twSamplerProgramModel::hasZone( std::uint32_t id ) const
{
    return impl_->program.findZone( id ) != nullptr;
}

bool twSamplerProgramModel::hasLayer( std::uint32_t id ) const
{
    return impl_->program.findLayer( id ).second != nullptr;
}

bool twSamplerProgramModel::value( twSamplerScope scope, std::uint32_t id,
                                   const std::string &attr, std::string *out ) const
{
    const nd::Program &p = impl_->program;
    switch( scope ) {
    case twSamplerScope::Program: {
        if( attr == "nextId" ) { if( out ) *out = std::to_string( p.nextId ); return true; }
        const nd::ProgramParam *d = nd::findProgramParam( attr );
        if( !d ) return false;
        if( out ) *out = nd::formatParam( *d, p );
        return true;
    }
    case twSamplerScope::Zone: {
        const nd::Zone *z = p.findZone( id );
        const nd::ZoneParam *d = nd::findZoneParam( attr );
        if( !z || !d ) return false;
        if( out ) *out = nd::formatParam( *d, *z );
        return true;
    }
    case twSamplerScope::Layer: {
        const nd::Layer *l = p.findLayer( id ).second;
        const nd::LayerParam *d = nd::findLayerParam( attr );
        if( !l || !d ) return false;
        if( out ) *out = nd::formatParam( *d, *l );
        return true;
    }
    }
    return false;
}

bool twSamplerProgramModel::zoneXml( std::uint32_t id, std::string *xml, std::size_t *index ) const
{
    const std::vector<nd::Zone> &zs = impl_->program.zones;
    for( std::size_t i = 0; i < zs.size(); ++i ) {
        if( zs[i].id != id ) continue;
        if( xml ) *xml = zoneText( zs[i] );
        if( index ) *index = i;
        return true;
    }
    return false;
}

bool twSamplerProgramModel::layerXml( std::uint32_t id, std::string *xml, std::uint32_t *zoneId,
                                      std::size_t *index ) const
{
    for( const nd::Zone &z : impl_->program.zones ) {
        for( std::size_t i = 0; i < z.layers.size(); ++i ) {
            if( z.layers[i].id != id ) continue;
            if( xml ) *xml = layerText( z.layers[i] );
            if( zoneId ) *zoneId = z.id;
            if( index ) *index = i;
            return true;
        }
    }
    return false;
}

twSamplerParamKind twSamplerProgramModel::paramKind( twSamplerScope scope, const std::string &attr )
{
    switch( scope ) {
    case twSamplerScope::Program: return kindOf( nd::findProgramParam( attr ) );
    case twSamplerScope::Zone:    return kindOf( nd::findZoneParam( attr ) );
    case twSamplerScope::Layer:   return kindOf( nd::findLayerParam( attr ) );
    }
    return twSamplerParamKind::Unknown;
}

std::vector<std::string> twSamplerProgramModel::paramNames( twSamplerScope scope )
{
    std::vector<std::string> out;
    switch( scope ) {
    case twSamplerScope::Program: for( const auto &d : nd::programParams() ) out.push_back( d.name ); break;
    case twSamplerScope::Zone:    for( const auto &d : nd::zoneParams() )    out.push_back( d.name ); break;
    case twSamplerScope::Layer:   for( const auto &d : nd::layerParams() )   out.push_back( d.name ); break;
    }
    return out;
}

namespace {
template <typename T>
bool rangeOf( const nd::ParamDesc<T> *p, double *lo, double *hi )
{
    if( !p || ( p->kind != nd::ParamKind::Int && p->kind != nd::ParamKind::Real ) ) return false;
    if( lo ) *lo = p->minValue;
    if( hi ) *hi = p->maxValue;
    return true;
}
template <typename T>
bool isIntParam( const nd::ParamDesc<T> *p ) { return p && p->kind == nd::ParamKind::Int; }
}  // namespace

bool twSamplerProgramModel::paramRange( twSamplerScope scope, const std::string &attr,
                                        double *lo, double *hi )
{
    switch( scope ) {
    case twSamplerScope::Program: return rangeOf( nd::findProgramParam( attr ), lo, hi );
    case twSamplerScope::Zone:    return rangeOf( nd::findZoneParam( attr ), lo, hi );
    case twSamplerScope::Layer:   return rangeOf( nd::findLayerParam( attr ), lo, hi );
    }
    return false;
}

bool twSamplerProgramModel::paramIsInteger( twSamplerScope scope, const std::string &attr )
{
    switch( scope ) {
    case twSamplerScope::Program: return isIntParam( nd::findProgramParam( attr ) );
    case twSamplerScope::Zone:    return isIntParam( nd::findZoneParam( attr ) );
    case twSamplerScope::Layer:   return isIntParam( nd::findLayerParam( attr ) );
    }
    return false;
}

// ------------------------------------------------------------ editing --------

bool twSamplerProgramModel::set( twSamplerScope scope, bool all,
                                 const std::vector<std::uint32_t> &ids,
                                 const std::vector<twSamplerEdit> &edits,
                                 std::vector<twSamplerPrior> *prior, std::string *error )
{
    if( edits.empty() ) {
        setError( error, "nothing to set" );
        return false;
    }
    nd::EditResult r;
    switch( scope ) {
    case twSamplerScope::Program:
        r = nd::setProgramParams( impl_->program, toCore( edits ) );
        break;
    case twSamplerScope::Zone:
        r = nd::setZones( impl_->program,
                          all ? nd::ZoneScope::everyZone() : nd::ZoneScope::zones( ids ),
                          toCore( edits ) );
        break;
    case twSamplerScope::Layer:
        if( all ) {
            setError( error, "the ALL scope is a zone scope (DESIGN §2.1)" );
            return false;
        }
        r = nd::setLayers( impl_->program, ids, toCore( edits ) );
        break;
    }
    if( !r.ok ) {
        setError( error, r.error );
        return false;
    }
    fromCore( r.prior, prior );
    return true;
}

bool twSamplerProgramModel::restore( twSamplerScope scope, const std::vector<twSamplerPrior> &prior,
                                     std::vector<twSamplerPrior> *replaced, std::string *error )
{
    nd::EditResult r;
    switch( scope ) {
    case twSamplerScope::Program: r = nd::restoreProgramParams( impl_->program, toCore( prior ) ); break;
    case twSamplerScope::Zone:    r = nd::restoreZones( impl_->program, toCore( prior ) ); break;
    case twSamplerScope::Layer:   r = nd::restoreLayers( impl_->program, toCore( prior ) ); break;
    }
    if( !r.ok ) {
        setError( error, r.error );
        return false;
    }
    fromCore( r.prior, replaced );
    return true;
}

std::uint32_t twSamplerProgramModel::addZone( const twSamplerAttrs &zone,
                                              const std::vector<twSamplerAttrs> &layers,
                                              std::string *error )
{
    if( (int)layers.size() > nd::kMaxLayers ) {
        setError( error, "a zone has at most " + std::to_string( nd::kMaxLayers ) + " layers" );
        return 0;
    }
    // AN EXPLICIT ZONE ID (QBX-265; proposal 56 §11, the author's answer of
    // 2026-11-01 to Q1 assumption c). Accepted only when it is free AND has
    // never been handed out, i.e. >= nextId; nextId then moves past it (the
    // core assigns the zone nextId, so setting nextId to the id first is all
    // it takes, and the layers follow it). The ids skipped over are never
    // handed out: they are below nextId from now on. Refused otherwise, with
    // a reason only that refusal produces.
    nd::Program  &p = impl_->program;
    std::uint32_t wantId = 0;
    twSamplerAttrs attrs;
    for( const auto &a : zone ) {
        if( a.first != "id" ) {
            attrs.push_back( a );
            continue;
        }
        char *end = nullptr;
        const unsigned long long v = std::strtoull( a.second.c_str(), &end, 10 );
        if( a.second.empty() || !end || *end != '\0' || a.second[0] == '-' || v == 0 ||
            v > 0xFFFFFFF0ull ) {
            setError( error, "zone id \"" + a.second + "\" is not a positive integer" );
            return 0;
        }
        wantId = (std::uint32_t)v;
    }
    nd::Zone z;
    if( !fillFromAttrs( nd::zoneParams(), attrs, z, "zone", error ) ) return 0;
    for( const twSamplerAttrs &la : layers ) {
        nd::Layer l;
        if( !fillFromAttrs( nd::layerParams(), la, l, "layer", error ) ) return 0;
        z.layers.push_back( std::move( l ) );
    }
    if( wantId != 0 ) {
        if( idInUse( p, wantId ) ) {
            setError( error, "zone id " + std::to_string( wantId ) + " is in use" );
            return 0;
        }
        if( wantId < p.nextId ) {
            setError( error, "zone id " + std::to_string( wantId ) +
                                 " was handed out before (nextId " + std::to_string( p.nextId ) +
                                 "); ids are never reused" );
            return 0;
        }
    }
    // THE OVERLAY RANGE (nassau-digital C8, DESIGN §8, decided 2026-11-01):
    // ids from nd::kOverlayIdFirst (1,000,000) on belong to the zones and
    // layers a library overlay adds, and a program never hands one out. The
    // zone and its layers take 1 + n consecutive ids from wantId (or nextId),
    // so the LAST of them must stay below the range. The core refuses such an
    // add too (nd::addZone returns 0); it is refused here first so the
    // refusal can say why, in a line no other path writes.
    const std::uint64_t first = wantId != 0 ? wantId : p.nextId;
    if( first + z.layers.size() >= (std::uint64_t)nd::kOverlayIdFirst ) {
        if( wantId != 0 )
            setError( error, "zone id " + std::to_string( wantId ) + " with " +
                                 std::to_string( z.layers.size() ) +
                                 " layer(s) reaches the ids reserved for library overlays (from " +
                                 std::to_string( nd::kOverlayIdFirst ) + ")" );
        else
            setError( error, "no ids left below the ids reserved for library overlays (nextId " +
                                 std::to_string( p.nextId ) + ", overlays from " +
                                 std::to_string( nd::kOverlayIdFirst ) + ")" );
        return 0;
    }
    const std::uint32_t before = p.nextId;
    if( wantId != 0 ) p.nextId = wantId;
    const std::uint32_t id = nd::addZone( p, std::move( z ) );
    if( id == 0 ) {
        p.nextId = before;
        setError( error, "the core refused the zone" );
    }
    return id;
}

std::uint32_t twSamplerProgramModel::addLayer( std::uint32_t zoneId, const twSamplerAttrs &layer,
                                               std::string *error )
{
    const nd::Zone *z = impl_->program.findZone( zoneId );
    if( !z ) {
        setError( error, "no zone with id " + std::to_string( zoneId ) );
        return 0;
    }
    if( (int)z->layers.size() >= nd::kMaxLayers ) {
        setError( error, "zone " + std::to_string( zoneId ) + " already has " +
                             std::to_string( nd::kMaxLayers ) + " layers" );
        return 0;
    }
    nd::Layer l;
    if( !fillFromAttrs( nd::layerParams(), layer, l, "layer", error ) ) return 0;
    // The overlay range, as in addZone(): the core refuses this add too.
    if( impl_->program.nextId >= nd::kOverlayIdFirst ) {
        setError( error, "no ids left below the ids reserved for library overlays (nextId " +
                             std::to_string( impl_->program.nextId ) + ", overlays from " +
                             std::to_string( nd::kOverlayIdFirst ) + ")" );
        return 0;
    }
    const std::uint32_t id = nd::addLayer( impl_->program, zoneId, std::move( l ) );
    if( id == 0 ) setError( error, "the core refused the layer" );
    return id;
}

bool twSamplerProgramModel::removeZone( std::uint32_t id, std::string *xml, std::size_t *index,
                                        std::string *error )
{
    nd::RemovedZone r = nd::removeZone( impl_->program, id );
    if( !r.ok ) {
        setError( error, "no zone with id " + std::to_string( id ) );
        return false;
    }
    if( xml ) *xml = zoneText( r.zone );
    if( index ) *index = r.index;
    return true;
}

bool twSamplerProgramModel::removeLayer( std::uint32_t id, std::string *xml, std::uint32_t *zoneId,
                                         std::size_t *index, std::string *error )
{
    nd::RemovedLayer r = nd::removeLayer( impl_->program, id );
    if( !r.ok ) {
        setError( error, "no layer with id " + std::to_string( id ) );
        return false;
    }
    if( xml ) *xml = layerText( r.layer );
    if( zoneId ) *zoneId = r.zoneId;
    if( index ) *index = r.index;
    return true;
}

bool twSamplerProgramModel::reinsertZone( const std::string &xml, std::size_t index,
                                          std::string *error )
{
    nd::Zone z;
    if( !parseZoneText( xml, z, error ) ) return false;
    nd::Program &p = impl_->program;
    std::vector<std::uint32_t> ids{ z.id };
    for( const nd::Layer &l : z.layers ) ids.push_back( l.id );
    for( std::uint32_t id : ids ) {
        if( idInUse( p, id ) ) {
            setError( error, "id " + std::to_string( id ) + " is in use" );
            return false;
        }
        if( id >= p.nextId ) {
            setError( error, "id " + std::to_string( id ) + " was never handed out (nextId " +
                                 std::to_string( p.nextId ) + ")" );
            return false;
        }
    }
    nd::RemovedZone r;
    r.ok    = true;
    r.index = std::min( index, p.zones.size() );
    r.zone  = std::move( z );
    if( !nd::reinsertZone( p, r ) ) {
        setError( error, "the core refused the reinsertion" );
        return false;
    }
    return true;
}

bool twSamplerProgramModel::reinsertLayer( const std::string &xml, std::uint32_t zoneId,
                                           std::size_t index, std::string *error )
{
    nd::Layer l;
    if( !parseLayerText( xml, l, error ) ) return false;
    nd::Program &p = impl_->program;
    nd::Zone *z = p.findZone( zoneId );
    if( !z ) {
        setError( error, "no zone with id " + std::to_string( zoneId ) );
        return false;
    }
    if( idInUse( p, l.id ) ) {
        setError( error, "id " + std::to_string( l.id ) + " is in use" );
        return false;
    }
    if( l.id >= p.nextId ) {
        setError( error, "id " + std::to_string( l.id ) + " was never handed out" );
        return false;
    }
    nd::RemovedLayer r;
    r.ok     = true;
    r.zoneId = zoneId;
    r.index  = std::min( index, z->layers.size() );
    r.layer  = std::move( l );
    if( !nd::reinsertLayer( p, r ) ) {
        setError( error, "the core refused the reinsertion (zone full?)" );
        return false;
    }
    return true;
}

static_assert( twSamplerProgramModel::kNumPads == nd::kNumPads, "the pad count is the core's" );

// --- the pads ----------------------------------------------------------------------

int twSamplerProgramModel::padKey( int n ) const
{
    return impl_->program.padKey( n );
}

std::vector<std::pair<int, int>> twSamplerProgramModel::padRows() const
{
    std::vector<std::pair<int, int>> out;
    for( const nd::PadRow &r : impl_->program.pads ) out.emplace_back( r.n, r.key );
    return out;
}

std::vector<int> twSamplerProgramModel::padsOnKey( int key ) const
{
    return nd::padsOnKey( impl_->program, key );
}

int twSamplerProgramModel::padSelectedByKey( int key, int currentPad ) const
{
    return nd::padSelectedByKey( impl_->program, key, currentPad );
}

std::vector<std::uint32_t> twSamplerProgramModel::zonesCoveringKey( int key ) const
{
    return nd::zonesCoveringKey( impl_->program, key );
}

bool twSamplerProgramModel::setPadKey( int n, int key, int *priorKey, std::string *error )
{
    const nd::PadEditResult r = nd::setPadKey( impl_->program, n, key );
    if( !r.ok ) {
        if( error ) *error = r.error;
        return false;
    }
    if( priorKey ) *priorKey = r.priorKey;
    return true;
}

// ------------------------------------------------------------ libraries ------

struct twSamplerLibrary::Impl {
    nd::Program base;
    nd::Overlay overlay;
};

twSamplerLibrary::twSamplerLibrary() : impl_( new Impl ) {}
twSamplerLibrary::twSamplerLibrary( const twSamplerLibrary &o ) : impl_( new Impl( *o.impl_ ) ) {}
twSamplerLibrary &twSamplerLibrary::operator=( const twSamplerLibrary &o )
{
    if( this != &o ) *impl_ = *o.impl_;
    return *this;
}
twSamplerLibrary::~twSamplerLibrary() = default;

bool twSamplerLibrary::fromXml( const std::string &baseXml, const std::string &overlayXml,
                                twSamplerLibrary &out, std::string *error )
{
    nd::ProgramParseResult b = nd::parseProgram( baseXml );
    if( !b.ok ) {
        setError( error, "the library program: " + b.error );
        return false;
    }
    nd::Overlay o;
    if( overlayXml.find_first_not_of( " \t\r\n" ) != std::string::npos ) {
        nd::OverlayParseResult r = nd::parseOverlay( overlayXml );
        if( !r.ok ) {
            setError( error, "the overlay: " + r.error );
            return false;
        }
        o = std::move( r.overlay );
    }
    out.impl_->base    = std::move( b.program );
    out.impl_->overlay = std::move( o );
    return true;
}

std::string twSamplerLibrary::baseXml() const { return nd::writeProgram( impl_->base ); }
std::string twSamplerLibrary::overlayXml() const { return nd::writeOverlay( impl_->overlay ); }

bool twSamplerLibrary::setBase( const std::string &baseXml, std::string *error )
{
    nd::ProgramParseResult b = nd::parseProgram( baseXml );
    if( !b.ok ) {
        setError( error, "the library program: " + b.error );
        return false;
    }
    impl_->base = std::move( b.program );
    return true;
}

void twSamplerLibrary::setBasePath( const std::string &path ) { impl_->overlay.base = path; }
std::string twSamplerLibrary::basePath() const { return impl_->overlay.base; }
std::string twSamplerLibrary::libraryVersion() const { return impl_->base.libraryVersion; }
std::string twSamplerLibrary::baseVersion() const { return impl_->overlay.baseVersion; }
std::size_t twSamplerLibrary::editCount() const { return impl_->overlay.edits.size(); }

twSamplerProgramModel twSamplerLibrary::applied() const
{
    twSamplerProgramModel m;
    m.impl_->program = nd::applyOverlay( impl_->base, impl_->overlay ).program;
    return m;
}

std::string twSamplerLibrary::report() const
{
    const nd::OverlayResult r = nd::applyOverlay( impl_->base, impl_->overlay );
    std::vector<std::string> parts;
    for( const nd::OverlayDrift &d : r.baseDrift ) parts.push_back( d.attr + " " + d.was + "->" + d.now );
    for( const nd::OverlayEditReport &e : r.edits ) {
        const std::string verb = e.index < impl_->overlay.edits.size() ? impl_->overlay.edits[e.index].name
                                                                      : std::string( "?" );
        std::string line = "#" + std::to_string( e.index ) + " " + verb + " ";
        switch( e.state ) {
        case nd::OverlayEditState::Applied: continue;
        case nd::OverlayEditState::Drifted:
            line += "drifted";
            for( const nd::OverlayDrift &d : e.drift ) line += " " + d.attr + " " + d.was + "->" + d.now;
            break;
        case nd::OverlayEditState::Orphaned: line += "orphaned " + std::to_string( e.missingId ); break;
        case nd::OverlayEditState::Refused:  line += "refused: " + e.error; break;
        case nd::OverlayEditState::Unknown:  line += "unknown"; break;
        }
        parts.push_back( line );
    }
    std::string out;
    for( const std::string &p : parts ) out += ( out.empty() ? "" : "; " ) + p;
    return out;
}

int twSamplerLibrary::drifted() const
{
    return (int)nd::applyOverlay( impl_->base, impl_->overlay ).drifted().size();
}

int twSamplerLibrary::orphaned() const
{
    return (int)nd::applyOverlay( impl_->base, impl_->overlay ).orphaned().size();
}

bool twSamplerLibrary::versionMismatch() const
{
    return nd::applyOverlay( impl_->base, impl_->overlay ).versionMismatch();
}

bool twSamplerLibrary::recordPad( int n, int key, int *priorKey, std::string *error )
{
    nd::Program current = nd::applyOverlay( impl_->base, impl_->overlay ).program;
    const int prior = current.padKey( n );
    const nd::RecordedEdit rec = nd::recordPadEdit( impl_->overlay, current, n, key );
    if( !rec.ok ) {
        setError( error, rec.error );
        return false;
    }
    if( priorKey ) *priorKey = prior;
    return true;
}

bool twSamplerLibrary::hidePad( int n, std::string *error )
{
    nd::Program current = nd::applyOverlay( impl_->base, impl_->overlay ).program;
    const nd::RecordedEdit rec = nd::recordHidePad( impl_->overlay, current, impl_->base, n );
    if( !rec.ok ) {
        setError( error, rec.error );
        return false;
    }
    return true;
}

int twSamplerLibrary::rebase()
{
    nd::Program current = nd::applyOverlay( impl_->base, impl_->overlay ).program;
    const nd::RebasedOverlay r = nd::rebaseOverlay( impl_->overlay, current, impl_->base );
    return (int)r.refreshed.size();
}

}  // namespace audio
