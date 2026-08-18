#ifndef SWAVEFORMDRAW_H
#define SWAVEFORMDRAW_H

#include <QColor>

#include "app/model/sobjectrenderer.h"   // SEnvelopeWindow, preview_t

class SObject;
class SLink;

// COLLECT: fill out[0..win.width) with obj's rendered-audio envelope over
// `win`, and return true; false when no preview could be produced. Peaks come
// from obj.getPreview(), which works for any SObject with a duration: a sample
// reads its data, a container (track/mixer) pulls its rendered output.
//
// The window is expressed in the caller's own time domain and the object's
// origin is `lk.getStartTime()`, exactly as the draw path has always folded it
// (a cut context folds its own window offset in before it gets here).
//
// This is the ONE probe-producing path — drawObjectWaveform() below is collect
// + draw over it (proposal 39 M1, D1), so the drawn waveform and anything that
// READS an envelope cannot disagree.
bool collectObjectEnvelope( SObject &obj, SLink &lk, const SEnvelopeWindow &win,
                            preview_t *out );

// NOTE: envelopeWindowOfContext() — the window drawObjectWaveform() derives
// from a render context — moved to app/model/sobjectrenderer.h at proposal 39
// M3, beside SEnvelopeWindow itself. The folder-sum overlay derives the same
// window for a LANE and objects/track may not include objects/wave, so the one
// spelling has to sit where both can reach it.

// Draw obj's rendered-audio waveform into ctx's visible rect, using ctx's time
// mapping for the horizontal scale (the caller's context already accounts for
// the link start and any cut window/offset). Returns false when no preview
// could be produced (so the caller can draw a placeholder).
bool drawObjectWaveform( SObject &obj, SLink &lk, SRenderContext &ctx,
                         const QColor &waveColor );

#endif // SWAVEFORMDRAW_H
