#ifndef _SSETCLIPFADEACTION_H_
#define _SSETCLIPFADEACTION_H_

#include "app/actions/saction.h"
#include "tw/events/twfade.h"
#include <QString>

/**
 * `set-clip-fade` — a clip's fade-in and fade-out (proposal 43 N5).
 *
 * ABSOLUTE, like `set-pitch` and unlike a nudge: the action carries the whole
 * fade and its inverse carries the whole previous one, so undo is exact and
 * two of them in a row do not compound.
 *
 * AUDIO ONLY. A fade is a gain shape and lives on `SCut`, exactly as pitch,
 * formant preservation and warp anchors do (cut/CONTRACT invariant 4); an
 * event clip is REFUSED rather than silently ignored.
 *
 * A take may be named, as everywhere else: `take` = -1 is the clip itself (or,
 * on a take column, its audible take), and the column resolves on BOTH
 * structural shapes through the generic seam (proposal 42 M2).
 *
 * THE AUTO-FADE TRI-STATE (plan 53 P5, §6.2). Since the project has an
 * auto-fade, an edge has three states and not two: an explicit fade, the
 * project's auto-fade, or a HARD edge the user asked for. The first two were
 * already expressible — a length, or no length — and `auto` is the third:
 *
 *     set-clip-fade clip="0,0" auto="0"      a hard edge at BOTH ends
 *     set-clip-fade clip="0,0" autoOut="0"   ...at the out edge only
 *     set-clip-fade clip="0,0" fadeIn="4800" an explicit fade, which wins at
 *                                            its own edge whatever `auto` says
 *
 * `auto` sets both flags; `autoIn` / `autoOut` override it per edge, and both
 * default to whatever `auto` says, which defaults to "unchanged". Two flags
 * rather than one because the auto-fade's own exemption (a SEAMLESS join,
 * D3b) is already decided per edge: a clip split in two has one seamless edge
 * and one ordinary one, so "not at the join, yes at the outer end" has to be
 * sayable.
 *
 * ABSOLUTE LIKE THE LENGTHS, and carried in the inverse the same way — so one
 * undo restores the previous fade AND the previous veto exactly.
 */
class SSetClipFadeAction : public SAction
{
public:
    SSetClipFadeAction() {}
    SSetClipFadeAction( const QString &clip, qint64 fadeIn, qint64 fadeOut,
                        twFadeShape shape, int take, bool autoIn = true,
                        bool autoOut = true );

    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "set-clip-fade" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString clip_;
    qint64 fadeIn_ = 0;
    qint64 fadeOut_ = 0;
    twFadeShape shape_ = twFadeShape::Linear;
    int take_ = -1;
    bool autoIn_  = true;
    bool autoOut_ = true;
};

#endif // _SSETCLIPFADEACTION_H_
