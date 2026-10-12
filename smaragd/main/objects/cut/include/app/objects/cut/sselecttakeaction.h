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
     * The COMP MAP to restore (proposal 43 N1), and the mark that THIS ACTION
     * IS AN INVERSE.
     *
     * `select-take` used to mean "this take for the WHOLE column", so it
     * cleared the map and its inverse had to put the map back, or one undo of
     * a click would silently destroy a comp. Since invariant 1' the forward
     * pass no longer always clears -- on a PARTIAL take it computes a map
     * (§4.3) -- so an inverse must be told apart from a forward pass rather
     * than inferred from the take it names. An inverse whose take happens to
     * be partial would otherwise recompute §4.3's map instead of restoring the
     * one it was handed, and undo would not be undo.
     *
     * Exactly one caller, the inverse this action builds, and it must keep
     * exactly one — the same discipline `SSetPluginParamAction::
     * setPreviousValue()` carries and for the same reason.
     */
    void setPreviousCompMap( const twCompMap &m )
    { prevMap_ = m; restoreMap_ = true; }

private:
    twCompMap prevMap_;
    /** Set only by setPreviousCompMap: restore `prevMap_` VERBATIM. */
    bool       restoreMap_ = false;
    QList<int> clipPath_;
    int        takeIndex_ = -1;
    // Edit groups: comp the SAME take index on every member's corresponding
    // stack (drum-kit comping); members without that take are skipped.
    bool       broadcast_ = true;
};

#endif // SSELECTTAKEACTION_H
