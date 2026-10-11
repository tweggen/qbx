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
// Inverse: remove-take at the inserted index (which collapses a stack back
// to a plain cut when one take remains — undoing the wrap).
class SAddTakeAction : public SAction {
public:
    SAddTakeAction() = default;
    SAddTakeAction( const QList<int> &clipPath, const QString &filePath,
                    offset_t startOffset = 0, int index = -1,
                    bool activate = true, const Fraction &stretch = Fraction(1),
                    double pitchCents = 0.0 );

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
};

#endif // SADDTAKEACTION_H
