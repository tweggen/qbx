#ifndef SPLACERECORDINGACTION_H
#define SPLACERECORDINGACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

class SCompositeAction;

// Action: place a finished recording onto a track — the multi-take verb
// (proposal 17 phase 2, decision 1). Plans the recording's span against the
// lane's existing columns and applies the plan as one atomic composite:
//
//   - a column (take stack or plain cut) the recording covers gets the
//     matching segment as a NEW take (slip = columnStart - recording start),
//     auto-activated; plain cuts are wrapped into stacks by add-take,
//   - uncovered spans become plain windowed cuts (place-clip),
//   - columns that begin BEFORE the recording start are left untouched
//     ("as applicable").
//
// An empty lane region therefore degenerates to today's behavior: one plain
// cut over the whole file. Undo removes everything the recording placed and
// unwraps any wrapped columns (composite of the children's inverses).
//
// `srcOffset` / `length` (proposal 21 L3b) place a SUB-RANGE of the file
// instead of all of it: `srcOffset` frames into the recording, `length` frames
// long (0 == to the end). That is what makes LOOP RECORDING fall out of this
// verb with no new machinery — one call per pass, all at the loop start, and
// the verb's own planner turns pass 2 onto pass 1's column as a TAKE (design
// D7, proposal 17's "phase 5"). Both default to "the whole file", so every
// existing call and every existing script are unchanged.
class SPlaceRecordingAction : public SAction {
public:
    SPlaceRecordingAction() = default;
    ~SPlaceRecordingAction() override;

    // The plan is HELD, so a child's inverse may borrow the child. See the
    // note on plan_ below; the copy a QList<SAction*> member would need is
    // not meaningful for an action that owns heap children.
    SPlaceRecordingAction( const SPlaceRecordingAction & ) = delete;
    SPlaceRecordingAction &operator=( const SPlaceRecordingAction & ) = delete;
    /**
     * `mode` arrives ALREADY RESOLVED, from the recorder (plan 53 §5.3). Empty
     * means "whatever this build would do anyway", which is what every call
     * written before plan 53 means and what every `.qxa` without `mode=` means.
     */
    SPlaceRecordingAction( const QList<int> &trackPath,
                           const QString &filePath, offset_t timePos,
                           offset_t srcOffset = 0, length_t length = 0,
                           const QString &mode = QString() );

    QString name() const override { return QStringLiteral("place-recording"); }
    QStringList knownAttributes() const override
    { return { QStringLiteral("trackPath"), QStringLiteral("filePath"),
               QStringLiteral("timePos"), QStringLiteral("srcOffset"),
               QStringLiteral("length"), QStringLiteral("mode"),
               QStringLiteral("armed") }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /**
     * The record mode this pass was planned under (plan 53 §5.2, QBX-156 M0).
     *
     * PARSED AND ROUND-TRIPPED ONLY IN M0 -- every value still runs the old
     * planner, so this attribute changes nothing about how a recording lands
     * yet. P1 is where it selects a planner.
     *
     * It arrives already RESOLVED: the recorder resolves per armed lane
     * (srecmode::resolveMode) and submits the verb with the answer, because
     * objects/cut may not depend on objects/track and so cannot resolve
     * anything itself (tools/check_layering.py). Empty means "the planner this
     * build would have used anyway", which is what every existing .qxa and
     * every recording made before plan 53 means.
     */
    const QString &recordMode() const { return mode_; }

    /**
     * Is this lane ARMED for the pass (plan 53 Q5)?
     *
     * `armed="0"` marks an UN-ARMED member of an armed lane's edit group: the
     * pass is submitted for it so that the takes modes can keep the group's
     * structure in step, without placing the file on it.
     *
     * **P1 does not enumerate them** (plan 53 §8, review 2 M5): the recorder
     * submits for armed lanes only, and every planner treats `armed="0"` as
     * nothing to do. P4 is what gives it meaning; the attribute exists from
     * here so that the verb's shape does not change when it does.
     */
    bool armed() const { return armed_; }

private:
    /**
     * THE PLAN THE PLANNER BUILT, kept alive for as long as this action is.
     *
     * It used to be a stack local in `apply()`, and that is a crash with
     * `replace` (QBX-172). `SRemoveSampleAction` cannot rebuild a take column
     * from parameters, so it PINS the content and hands back an
     * `SRestoreTakeColumnAction` that BORROWS it -- `owner_`, documented in
     * that header as "holds the pin; borrowed". A stack composite is destroyed
     * when `apply()` returns, which deletes the `remove-sample` child, drops
     * the pin, and leaves the inverse on the undo stack pointing at freed
     * memory; the first undo then reads it. Measured as a SIGBUS in
     * `rec_replace_cells`.
     *
     * Holding it here makes the lifetimes right by construction, because
     * `SActionUndoCommand` owns the FORWARD action and the inverse together
     * ("ownership: kept for redo" / "kept for undo") and destroys them
     * together, so no inverse can outlive the child it borrows from.
     *
     * It is NOT a second source of truth: nothing reads it back, it is
     * rebuilt from scratch on every apply (a redo re-plans against the lane
     * as the undo left it), and the previous one is deleted there.
     *
     * The alternative -- giving `SRestoreTakeColumnAction` its own pin -- is
     * the better long-term shape and is a change to two public contracts in
     * this module, so it is deliberately NOT folded into this ticket.
     */
    SCompositeAction *plan_ = nullptr;
    QString    mode_;        // "" = whatever this build would do anyway
    bool       armed_ = true;
    QList<int> trackPath_;
    QString    filePath_;
    offset_t   timePos_ = 0;
    offset_t   srcOffset_ = 0;
    length_t   length_ = 0;      // 0 == to the end of the file
};

#endif // SPLACERECORDINGACTION_H
