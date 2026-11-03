#include "app/samplerui/ssamplerzones.h"

#include "tw/sampler/twsamplerprogrammodel.h"

#include <QStringList>

#include <algorithm>

using audio::twSamplerProgramModel;
using audio::twSamplerScope;

namespace {

// The core writes every value; an Int is plain decimal text. QString::toInt
// is locale-independent (the C locale), unlike strtol under a German
// LC_NUMERIC would be for a Real (the qbx number-parsing rule).
int intValue( const twSamplerProgramModel &m, twSamplerScope scope, std::uint32_t id,
              const char *attr, int fallback )
{
    std::string text;
    if( !m.value( scope, id, attr, &text ) ) return fallback;
    bool ok = false;
    const int v = QString::fromStdString( text ).toInt( &ok );
    return ok ? v : fallback;
}

QString textValue( const twSamplerProgramModel &m, twSamplerScope scope, std::uint32_t id,
                   const char *attr )
{
    std::string text;
    return m.value( scope, id, attr, &text ) ? QString::fromStdString( text ) : QString();
}

}  // namespace

namespace ssamplerzones {

QVector<QPair<int, int>> velocityGaps( int zoneVelLo, int zoneVelHi,
                                       const QVector<LayerInfo> &layers )
{
    QVector<QPair<int, int>> out;
    if( zoneVelHi < zoneVelLo ) return out;
    int v = zoneVelLo;
    while( v <= zoneVelHi ) {
        // The furthest velocity reachable from v through windows covering v.
        int coveredTo = -1;
        for( const LayerInfo &l : layers ) {
            const int lo = std::max( l.velLo, zoneVelLo );
            const int hi = std::min( l.velHi, zoneVelHi );
            if( lo <= v && v <= hi ) coveredTo = std::max( coveredTo, hi );
        }
        if( coveredTo >= v ) {
            v = coveredTo + 1;
            continue;
        }
        // v is a gap: it runs to the first velocity a window starts at.
        int next = zoneVelHi + 1;
        for( const LayerInfo &l : layers ) {
            const int lo = std::max( l.velLo, zoneVelLo );
            const int hi = std::min( l.velHi, zoneVelHi );
            if( lo > v && lo <= hi ) next = std::min( next, lo );
        }
        out.append( qMakePair( v, next - 1 ) );
        v = next;
    }
    return out;
}

QVector<ZoneInfo> zones( const twSamplerProgramModel &m )
{
    QVector<ZoneInfo> out;
    for( std::uint32_t id : m.zoneIds() ) {
        ZoneInfo z;
        z.id      = id;
        z.keyLo   = intValue( m, twSamplerScope::Zone, id, "keyLo", 0 );
        z.keyHi   = intValue( m, twSamplerScope::Zone, id, "keyHi", 127 );
        z.velLo   = intValue( m, twSamplerScope::Zone, id, "velLo", 0 );
        z.velHi   = intValue( m, twSamplerScope::Zone, id, "velHi", 127 );
        z.rootKey = intValue( m, twSamplerScope::Zone, id, "rootKey", 60 );
        for( std::uint32_t lid : m.layerIds( id ) ) {
            LayerInfo l;
            l.id    = lid;
            l.velLo = intValue( m, twSamplerScope::Layer, lid, "velLo", 0 );
            l.velHi = intValue( m, twSamplerScope::Layer, lid, "velHi", 127 );
            l.src   = textValue( m, twSamplerScope::Layer, lid, "src" );
            z.layers.append( l );
        }
        z.gaps = velocityGaps( z.velLo, z.velHi, z.layers );
        out.append( z );
    }
    return out;
}

QString keyName( int key )
{
    static const char *const kNames[12] = { "C",  "C#", "D",  "D#", "E",  "F",
                                            "F#", "G",  "G#", "A",  "A#", "B" };
    if( key < 0 || key > 127 ) return QStringLiteral( "-" );
    return QString::fromLatin1( kNames[key % 12] ) + QString::number( key / 12 - 1 );
}

}  // namespace ssamplerzones
