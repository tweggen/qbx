#ifndef SSELECTTAKEACTION_H
#define SSELECTTAKEACTION_H

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include <QList>

// Action: select the audible take of a take stack (proposal 17) — the
// comping gesture. `clip` addresses the stack placement (track-path + link
// index), `take` is the take index (-1 = none audible). Rejected when the
// clip is not a take stack or the index is out of range.
//
// Inverse: select-take with the previously active index.
class SSelectTakeAction : public SAction {
public:
    SSelectTakeAction() = default;
    SSelectTakeAction( const QList<int> &clipPath, int takeIndex,
                       bool broadcast = true );

    QString name() const override { return QStringLiteral("select-take"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /**
     * The COMP MAP to restore (proposal 43 N1). `select-take` means "this take
     * for the WHOLE column", so it CLEARS the map — and its inverse has to put
     * it back, or one undo of a click would silently destroy a comp.
     *
     * Exactly one caller, the inverse this action builds, and it must keep
     * exactly one — the same discipline `SSetPluginParamAction::
     * setPreviousValue()` carries and for the same reason.
     */
    void setPreviousCompMap( const twCompMap &m ) { prevMap_ = m; }

private:
    twCompMap prevMap_;
    QList<int> clipPath_;
    int        takeIndex_ = -1;
    // Edit groups: comp the SAME take index on every member's corresponding
    // stack (drum-kit comping); members without that take are skipped.
    bool       broadcast_ = true;
};

#endif // SSELECTTAKEACTION_H
