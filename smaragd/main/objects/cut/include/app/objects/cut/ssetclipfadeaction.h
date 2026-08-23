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
 */
class SSetClipFadeAction : public SAction
{
public:
    SSetClipFadeAction() {}
    SSetClipFadeAction( const QString &clip, qint64 fadeIn, qint64 fadeOut,
                        twFadeShape shape, int take );

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
};

#endif // _SSETCLIPFADEACTION_H_
