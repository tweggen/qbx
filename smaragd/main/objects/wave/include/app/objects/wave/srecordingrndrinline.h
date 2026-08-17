#ifndef _SRECORDING_RNDR_INLINE_H_
#define _SRECORDING_RNDR_INLINE_H_

#include <QObject>
#include "app/model/sobjectrenderer.h"

class SRecordingContent;

/**
 * The inline renderer of a GROWING recording (proposal 21 L3b, design D7).
 *
 * The waveform is the ordinary shared draw loop over the content's own
 * incremental peak ladder — the same `drawObjectWaveform` an `SPlainWave`
 * uses, so a recording in progress and the file it becomes are drawn by one
 * piece of code. What is added is THE FRONTIER: a bright vertical rule at the
 * last captured frame, which is the one thing on screen that says "this clip
 * is still being written". It is drawn HERE rather than in
 * `SCutRendererInline` because it belongs to the content (the capture), not to
 * the window: two cuts over one capture both show it, and no existing clip
 * gains a branch it would have to skip.
 */
class SRecordingRendererInline
    : public SObjectRenderer
{
    Q_OBJECT
public:
    explicit SRecordingRendererInline( SRecordingContent &c )
        : SObjectRenderer( (SObject &) c ) {}
    ~SRecordingRendererInline() override {}

    void draw( SLink &, SRenderContext & ) override;
    SRecordingContent &getRecording() const
    { return (SRecordingContent &) getObject(); }
};

#endif // _SRECORDING_RNDR_INLINE_H_
