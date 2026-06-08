#ifndef SWAVEFORMDRAW_H
#define SWAVEFORMDRAW_H

#include <QColor>

class SObject;
class SLink;
class SRenderContext;

// Draw obj's rendered-audio waveform into ctx's visible rect, using ctx's time
// mapping for the horizontal scale (the caller's context already accounts for
// the link start and any cut window/offset). Peaks come from obj.getPreview(),
// which works for any SObject with a duration: a sample reads its data, a
// container (track/mixer) pulls its rendered output. Returns false when no
// preview could be produced (so the caller can draw a placeholder).
bool drawObjectWaveform( SObject &obj, SLink &lk, SRenderContext &ctx,
                         const QColor &waveColor );

#endif // SWAVEFORMDRAW_H
