#ifndef _SAUDIOPASSVERBS_H_
#define _SAUDIOPASSVERBS_H_

#include "app/objects/recplan/srecordplanner.h"
#include <QString>

/**
 * THE AUDIO HALF OF `srecplan::PassVerbs` (plan 53 §5.1, QBX-188).
 *
 * `objects/recplan` decides what a recording pass must do to a lane; this says
 * it in `objects/cut`'s own vocabulary — `place-clip`, `add-take`,
 * `split-clip`, `remove-sample`, `merge-columns`. The planner used to name
 * those verbs directly, which is what put it out of the EVENT side's reach:
 * `objects/midi` may not depend on `objects/cut` (proposal 37 §3.5), so one
 * planner serving both kinds needs the concrete verbs behind a seam.
 *
 * IT OWNS THE SOURCE ARITHMETIC, and that is the point of the split. The
 * planner hands over TIMELINE positions; turning "the pass's material begins
 * at timeline position `takeStart`" into a frame offset into a WAV is
 * `srcOffset + (takeStart - a)`, and it is audio's answer alone. The event
 * implementation turns the same question into a tick rebased through the
 * clip's own window.
 *
 * Stateless, so one instance can serve every armed lane of a pass; the
 * payload it needs is the `srecplan::Span` the planner passes back in.
 */
class SAudioPassVerbs final : public srecplan::PassVerbs
{
public:
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
    SAction *nameTake( const QList<int> &clipPath,
                       const QString &name ) const override;
    SAction *mergeColumns(
        const QList<QList<int>> &clipPaths,
        offset_t extentStart = 0,
        length_t extentLen = -1 ) const override;
};

#endif // _SAUDIOPASSVERBS_H_
