// twSamplerProgramModel: the editable NassauDigital program, a thin forwarder
// to the core's model, edits and XML (proposal 56 §4.4, Q1).

#include "tw/sampler/twsamplerprogrammodel.h"

#include "twsamplerimpl.h"

#include "nd_edit.h"
#include "nd_program.h"
#include "nd_program_xml.h"
#include "nd_xml.h"

#include <algorithm>
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
// table does not know is REFUSED here (a script typo), and so is `id`.
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
    nd::Zone z;
    if( !fillFromAttrs( nd::zoneParams(), zone, z, "zone", error ) ) return 0;
    for( const twSamplerAttrs &la : layers ) {
        nd::Layer l;
        if( !fillFromAttrs( nd::layerParams(), la, l, "layer", error ) ) return 0;
        z.layers.push_back( std::move( l ) );
    }
    const std::uint32_t id = nd::addZone( impl_->program, std::move( z ) );
    if( id == 0 ) setError( error, "the core refused the zone" );
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

}  // namespace audio
