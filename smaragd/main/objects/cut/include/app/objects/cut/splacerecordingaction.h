#ifndef SPLACERECORDINGACTION_H
#define SPLACERECORDINGACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

class SCompositeAction;

// Action: place a finished recording onto a track — the multi-take verb
// (proposal 17 phase 2, decision 1).
//
// A THIN VERB since QBX-171 (plan 53 §5.1): it reads the lane ONCE into an
// `srecplan::LaneSnapshot`, picks the `srecplan::Planner` for the mode it was
// HANDED, and applies whatever that planner appended to one atomic composite.
// It decides nothing itself, and a planner emits only existing verbs, so every
// mode's undo is the composite's inverse chain. What each mode does is in
// `srecordplanner.h`; the default `takes` is plan 53's MODE A since QBX-180 —
// one union column per pass per lane — and an empty lane region still
// degenerates to one plain cut over the file, as every build has done.
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
                           const QString &mode = QString(),
                           length_t extent = 0 );

    QString name() const override { return QStringLiteral("place-recording"); }
    QStringList knownAttributes() const override
    { return { QStringLiteral("trackPath"), QStringLiteral("filePath"),
               QStringLiteral("timePos"), QStringLiteral("srcOffset"),
               QStringLiteral("length"), QStringLiteral("mode"),
               QStringLiteral("armed"), QStringLiteral("extent") }; }
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

    /**
     * THE TAKE'S EXTENT, when the recorder knows it is WIDER than the material
     * (plan 53 §3.1 step 5, QBX-180). `0` = the placed span.
     *
     * It exists because `length` cannot say this: `length` bounds the MATERIAL
     * and this bounds the TAKE. A cycle pass is placed at the cycle start and
     * asked for the whole cycle length, but the span is clamped to what was
     * actually captured (`min( length, waveDur - srcOffset )`), so the last
     * pass of essentially every cycle recording is SHORT -- and under mode A
     * its take must still be an alternative for the same bars as the full
     * passes beside it, or `select-take` cannot isolate it (A0b made the click
     * extent-aware). The recorder passes the cycle length and 0 otherwise.
     *
     * It replaces the `srecplan::Span::fromCycle` flag QBX-171 left unread: a
     * boolean cannot say where the cycle ENDS.
     */
    length_t extent() const { return extent_; }

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
    length_t   extent_ = 0;      // 0 == the placed span
};

#endif // SPLACERECORDINGACTION_H
