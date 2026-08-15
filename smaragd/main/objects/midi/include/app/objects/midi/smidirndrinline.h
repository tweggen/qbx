#ifndef _SMIDIRNDRINLINE_H_
#define _SMIDIRNDRINLINE_H_

#include "app/model/sobjectrenderer.h"

class SMidiCut;
class SMidiSequence;

/**
 * The event-clip thumbnail (proposal 36 6.1).
 *
 * It joins the arranger through the existing polymorphic path
 * (`STrackRendererInline` -> `lk->getSObject().getInlineRenderer()->draw()`,
 * timeline invariant 2) - there is nothing to register and no canvas code to
 * change. Note rectangles are scaled to the PRESENT pitch range of what is
 * visible (a two-note clip should not draw two hairlines at the bottom of a
 * 128-semitone axis), controllers are faint horizontal marks, and metadata is
 * a tick strip along the top edge, mirroring the onset strip.
 */
class SMidiSequenceRendererInline : public SObjectRenderer
{
    Q_OBJECT
public:
    explicit SMidiSequenceRendererInline( SMidiSequence & );
    void draw( SLink &, SRenderContext & ) override;
    SMidiSequence &sequence() const;
};

/**
 * The WINDOW's renderer: the same painting, but only the notes this clip's
 * window admits, positioned through the clip's own tick->frame mapping. It is
 * the SCutRendererInline analogue and, like it, is what the lane actually
 * calls (the placement's object is the cut, not the sequence).
 */
class SMidiCutRendererInline : public SObjectRenderer
{
    Q_OBJECT
public:
    explicit SMidiCutRendererInline( SMidiCut & );
    void draw( SLink &, SRenderContext & ) override;
    SMidiCut &cut() const;
};

#endif // _SMIDIRNDRINLINE_H_
