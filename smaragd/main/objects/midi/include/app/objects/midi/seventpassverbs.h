#ifndef _SEVENTPASSVERBS_H_
#define _SEVENTPASSVERBS_H_

#include <QList>
#include <QString>
#include <vector>

#include "app/objects/recplan/srecordplanner.h"
#include "app/objects/midi/smidisequence.h"

class SProject;

/**
 * THE EVENT HALF OF `srecplan::PassVerbs` (plan 53 P3, QBX-181).
 *
 * `objects/recplan` decides what a recording pass must do to a lane;
 * `SAudioPassVerbs` says it in `objects/cut`'s vocabulary and this says it in
 * `objects/midi`'s. Between them they are the whole reason the planner moved
 * out of `objects/cut` (QBX-188): one copy of the union rule, two content
 * kinds, and neither module depending on the other.
 *
 * ## WHAT IT CARRIES, AND WHY IT HAS TO
 *
 * `Span` is the AUDIO payload — a file path and a frame offset into it — and
 * says nothing about notes. So the event implementation carries the pass
 * itself: the event table in PASS-WINDOW TICKS (tick 0 = `span.a`), the pass's
 * tick length, and the name the clips get. One instance per pass per lane,
 * borrowed by `Context::verbs`, exactly as that field documents.
 *
 * ## IT OWNS THE SOURCE ARITHMETIC, which is the point of the seam
 *
 * The planner hands over TIMELINE positions. Audio turns "the pass's material
 * begins at timeline `at`" into `srcOffset + (at − a)` frames into a WAV; this
 * turns it into a tick offset into the pass's own event table, and rebases
 * every event by it. A planner doing that itself would be doing it in one
 * domain for both.
 *
 * ## TWO VERBS COME THROUGH `SActionRegistry`, BY NAME
 *
 * `split-clip` and `merge-columns` are kind-agnostic already — the first is
 * gated on an event clip by `midi_clip_edit_verbs`, and the second folds
 * `insert-midi-clip`s into a column of event takes — but they live in
 * `objects/cut`, which this module may not see (proposal 37 §3.5). They are
 * therefore built through the registry and configured from their own XML form,
 * which is exactly the precedent `makeAddTrack()` in `smidifileactions.cpp`
 * set in this same module for this same reason: "a verb, not a type".
 *
 * ## `removeChild` IS UNREACHABLE, AND SAYS SO
 *
 * Only `takes` and `takes-split` reach a planner on the event side. Plan 53
 * §3.3 says in as many words that for MIDI `overdub` and `replace` keep their
 * existing meanings — "the pass is MERGED into the active take", "notes inside
 * the pass window are deleted from the active take, CCs survive" — and those
 * are not the audio planners' meanings at all, so `place-midi-recording` keeps
 * its own two bodies for them. `removeChild` is `Replace`'s verb and nothing
 * else's, so it refuses loudly rather than guessing at a removal nobody asked
 * for. If `replace` ever does route through the planner for events, it needs a
 * removal verb that captures an event clip's whole state in its own `apply()`:
 * `remove-sample` cannot restore one (not file-backed, not container-backed)
 * and `SRemoveMidiClipAction` wants that state at construction time.
 */
class SEventPassVerbs final : public srecplan::PassVerbs
{
public:
    /**
     * `passTicks` is the pass window's musical length and `events` are in its
     * ticks (tick 0 = `Span::a`), which is `place-midi-recording`'s own
     * contract for the table it is handed.
     */
    SEventPassVerbs( SProject *project, const QString &pathRoot,
                     qint64 passTicks, std::vector<SEvent> events,
                     const QString &clipName );

    SAction *placePass( const QList<int> &lanePath,
                        const srecplan::Span &span,
                        offset_t at, length_t len ) const override;
    SAction *addTakeFromPass( const QList<int> &clipPath,
                              const srecplan::Span &span,
                              offset_t takeStart ) const override;
    SAction *splitClip( const QList<int> &clipPath,
                        offset_t at ) const override;
    SAction *removeChild( const QList<int> &lanePath, int index,
                          offset_t at ) const override;
    SAction *mergeColumns(
        const QList<QList<int>> &clipPaths ) const override;

private:
    /**
     * A frame LENGTH as a tick length, through THE tempo map
     * (POSITION_DOMAINS rule 7). Signed: the planner may hand over a take
     * anchor up to two frames BEFORE the pass began (`pieceCanSplitAt`), and
     * the tick offset then goes negative — leading silence, the event analogue
     * of audio's negative `srcOffset` (legal since proposal 23).
     */
    qint64 ticksOf_( int64_t frames ) const;

    SProject           *project_ = nullptr;
    QString             pathRoot_;
    qint64              passTicks_ = 0;
    std::vector<SEvent> events_;
    QString             clipName_;
};

#endif // _SEVENTPASSVERBS_H_
