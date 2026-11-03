#ifndef SSAMPLERZONES_H
#define SSAMPLERZONES_H

// ssamplerzones.h — what the editor's views DRAW, read from the core's program
// (QBX-279; nassau-digital DESIGN §2, §7).
//
// Pure functions over a twSamplerProgramModel: the views paint from them and
// SSamplerEditor::describe() reports from them, so what a gate reads is what a
// view draws. Every value comes from twSamplerProgramModel::value() (the core
// writes it, this parses the core's text with the C locale), never from a
// second copy of the program.
//
// THE VELOCITY GAP (DESIGN §2): "a velocity inside the zone that no layer
// covers plays nothing, and the Map view marks the gap; the editor never
// invents a fill". A layer's window is clamped into its zone's (the core's
// pickLayers reads the layer's own velLo..velHi, and the zone's bounds keep a
// note outside the zone from reaching it at all), so the gaps of a zone are
// the velocities in [zone.velLo, zone.velHi] outside every layer's window. A
// zone with no layer is one gap over its whole range.

#include <QPair>
#include <QString>
#include <QVector>

#include <cstdint>

namespace audio {
class twSamplerProgramModel;
}

namespace ssamplerzones {

struct LayerInfo {
    std::uint32_t id = 0;
    int           velLo = 0;
    int           velHi = 127;
    QString       src;
};

struct ZoneInfo {
    std::uint32_t      id = 0;
    int                keyLo = 0;
    int                keyHi = 127;
    int                velLo = 0;
    int                velHi = 127;
    int                rootKey = 60;
    QVector<LayerInfo> layers;
    // Velocity ranges [lo, hi] inside the zone that no layer covers.
    QVector<QPair<int, int>> gaps;

    bool coversKey( int key ) const { return key >= keyLo && key <= keyHi; }
    bool covers( int key, int vel ) const
    { return coversKey( key ) && vel >= velLo && vel <= velHi; }
};

// Every zone in program order.
QVector<ZoneInfo> zones( const audio::twSamplerProgramModel &m );

// The gaps of one zone from its bounds and its layers' windows (the rule above).
QVector<QPair<int, int>> velocityGaps( int zoneVelLo, int zoneVelHi,
                                       const QVector<LayerInfo> &layers );

// "C-1" .. "G9": MIDI key 60 is C4.
QString keyName( int key );

}  // namespace ssamplerzones

#endif
