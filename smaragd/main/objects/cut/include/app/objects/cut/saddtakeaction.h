#ifndef SADDTAKEACTION_H
#define SADDTAKEACTION_H

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include "tw/core/twfraction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

// Action: add a take (a media file) to a clip (proposal 17). `clip` may
// address a take stack (the take is inserted) or a plain SCut (it is first
// wrapped into a stack, becoming take 0). The new take's cut is windowed to
// the stack duration; `startOffset` slips it, `stretch`/`pitchCents` seed
// its grain params (also used to restore a removed take's window on undo).
// `index` = -1 appends; `activate` = 1 (default) makes the new take audible
// (decision 1: newest take auto-activated).
//
// THE TAKE'S EXTENT (plan 53 §4.1, invariant 1′): `at` is where the take's
// material starts in COLUMN frames and `length` how far it reaches, so a take
// may cover LESS than its column. The defaults are the whole column -- `at` 0
// and `length` -1, meaning "the column's duration" -- which is what every
// caller and every .qxa written before extents means, and what keeps them all
// correct unchanged.
//
// They also carry a REMOVED take's extent back on undo. Without them the
// inverse of a `remove-take` on a partial take restored a FULL-extent one,
// which is a different column wearing the same take count.
//
// Inverse: remove-take at the inserted index (which collapses a stack back
// to a plain cut when one FULL-EXTENT take remains — undoing the wrap;
// invariant 3′ keeps a partial one as a column).
class SAddTakeAction : public SAction {
public:
    SAddTakeAction() = default;
    SAddTakeAction( const QList<int> &clipPath, const QString &filePath,
                    offset_t startOffset = 0, int index = -1,
                    bool activate = true, const Fraction &stretch = Fraction(1),
                    double pitchCents = 0.0,
                    offset_t at = 0, length_t length = -1 );

    QString name() const override { return QStringLiteral("add-take"); }

    /**
     * RESTORE this comp map instead of clearing one (plan 53 B10).
     *
     * `add-take activate=1` normally CLEARS the map, because activating a take
     * while a map decides every position would badge a take nobody can hear
     * (QBX-159). But when this action is the INVERSE of a `remove-take`, the
     * map it has to put back is the one that existed before the removal --
     * complete, with the removed take's segments in it -- and that is not a
     * cleared map and not a renumbered one either.
     *
     * Inverse-only and unserialized, like SRemoveTakeAction's twin of it.
     */
    void setRestoreCompMap( const twCompMap &map )
    { restoreMap_ = map; hasRestoreMap_ = true; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    twCompMap  restoreMap_;
    bool       hasRestoreMap_ = false;
    QList<int> clipPath_;
    QString    filePath_;
    offset_t   startOffset_ = 0;
    int        index_ = -1;
    bool       activate_ = true;
    Fraction   stretch_ = Fraction(1);
    double     pitchCents_ = 0.0;
    offset_t   at_ = 0;          // extent start, column frames
    length_t   length_ = -1;     // extent length; < 0 = the whole column
};

#endif // SADDTAKEACTION_H
