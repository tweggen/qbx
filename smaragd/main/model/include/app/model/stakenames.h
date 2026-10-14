#ifndef _STAKENAMES_H_
#define _STAKENAMES_H_

#include <QRegularExpression>
#include <QString>

/**
 * THE RECORDER'S TAKE NAME, and the one test for "this take came from a
 * recording" (plan 53 §8 P4, D5, QBX-182).
 *
 * `"Take 3 · 14:27"` — the take's number in its column, then the wall-clock
 * minute the pass was committed. The user sees it on the clip body and in the
 * take lane; two other things read it, which is why the format lives in ONE
 * place:
 *
 *  - `select-take take="newest"` (§6.5) needs to tell a RECORDED take from one
 *    that came from the user's arrangement, because "a take without a stamp is
 *    older than any with one";
 *  - `cleanup-takes` (§6.3, decision D5 (a)) exempts takes that did NOT come
 *    from a recording, and `isRecorded()` is that origin marker.
 *
 * IN `app/model` AND NOT IN `objects/cut`, although every consumer so far is a
 * `cut` verb: `place-midi-recording` names an EVENT take through the same
 * format and `objects/midi` may not depend on `objects/cut`
 * (`tools/check_layering.py`, proposal 37 §3.5). A take name is a model-level
 * string convention, not an audio one.
 *
 * IT IS A HEURISTIC AND IS MEANT TO BE ONE. A user may name a clip
 * `"Take 2 · 14:27"` by hand and it will read as a recording; the alternative
 * — a serialized per-take origin flag — is a file-format change D5 did not
 * ask for, and the cost of a false positive is that `cleanup-takes` offers to
 * remove one clip the user would have kept, which its confirmation lists by
 * name.
 *
 * ORDER IS NOT READ OFF THE STAMP. `HH:MM` has no date and two passes in one
 * minute tie, so `newest` orders recorded takes by their INDEX in the column,
 * which is recording order because a recorded take is always appended last
 * (`merge-columns` takes the pass as its final operand, `add-take` appends).
 * The stamp says WHETHER, the index says WHICH.
 */
namespace stakenames {

/** `"Take <n> · <stamp>"`. `n` is 1-based, as the take lane numbers them. */
inline QString recorded( int takeNumber, const QString &stamp )
{
    return QStringLiteral( "Take %1 · %2" )
        .arg( takeNumber ).arg( stamp );
}

/** Does `name` carry the recorder's stamp? */
inline bool isRecorded( const QString &name )
{
    // Anchored at both ends, and the stamp half is not parsed: what matters is
    // that the recorder wrote it, not what time it says.
    static const QRegularExpression re(
        QStringLiteral( "\\ATake \\d+ · .+\\z" ) );
    return re.match( name ).hasMatch();
}

}  // namespace stakenames

#endif // _STAKENAMES_H_
