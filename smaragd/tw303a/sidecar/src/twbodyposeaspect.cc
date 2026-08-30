#include "tw/sidecar/twbodyposeaspect.h"

#include <cstring>

namespace {

void putF64( std::vector<uint8_t> &o, double v )
{
    uint64_t b; std::memcpy( &b, &v, 8 );
    for( int i = 0; i < 8; i++ ) o.push_back( (uint8_t) ( b >> ( i * 8 ) ) );
}

void putF32( std::vector<uint8_t> &o, float v )
{
    uint32_t b; std::memcpy( &b, &v, 4 );
    for( int i = 0; i < 4; i++ ) o.push_back( (uint8_t) ( b >> ( i * 8 ) ) );
}

float getF32( const uint8_t *p )
{
    uint32_t b = (uint32_t) p[0] | ( (uint32_t) p[1] << 8 )
               | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
    float v; std::memcpy( &v, &b, 4 );
    return v;
}

constexpr int kDof    = (int) twBodyPoseDof::Count;
constexpr int kStride = kDof * 3 * 4;

} // namespace

void twBodyPoseParams::serialize( std::vector<uint8_t> &out ) const
{
    // THE GROOVE BLOB, VERBATIM AND FIRST. Nesting, never extending -- see the
    // header. This call is the ONLY relationship proposal 44 has with the
    // groove params serializer, and it is read-only.
    groove.serialize( out );
    putF64( out, body.massKg );
    putF64( out, body.statureM );
}

void twBodyPoseEncode( const std::vector<twBodyPoseRecord> &records,
                       std::vector<uint8_t> &out )
{
    out.clear();
    out.reserve( records.size() * (size_t) kStride );
    for( const twBodyPoseRecord &r : records )
        for( int d = 0; d < kDof; d++ ) {
            putF32( out, r.dof[d].angle );
            putF32( out, r.dof[d].velocity );
            putF32( out, r.dof[d].muscleTorque );
        }
}

std::vector<twBodyPoseRecord> twBodyPoseDecode( const uint8_t *payload,
                                                uint64_t payloadLen )
{
    std::vector<twBodyPoseRecord> out;
    if( !payload || payloadLen == 0 || ( payloadLen % (uint64_t) kStride ) != 0 )
        return out;
    const uint64_t n = payloadLen / (uint64_t) kStride;
    out.resize( (size_t) n );
    for( uint64_t i = 0; i < n; i++ ) {
        const uint8_t *p = payload + i * (uint64_t) kStride;
        for( int d = 0; d < kDof; d++ ) {
            out[(size_t) i].dof[d].angle        = getF32( p + ( d * 3 + 0 ) * 4 );
            out[(size_t) i].dof[d].velocity     = getF32( p + ( d * 3 + 1 ) * 4 );
            out[(size_t) i].dof[d].muscleTorque = getF32( p + ( d * 3 + 2 ) * 4 );
        }
    }
    return out;
}
