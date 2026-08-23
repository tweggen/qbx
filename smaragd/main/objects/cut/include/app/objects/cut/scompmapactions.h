#ifndef _SCOMPMAPACTIONS_H_
#define _SCOMPMAPACTIONS_H_

#include "app/actions/saction.h"
#include "tw/events/twcompmap.h"
#include <QString>

/**
 * THE COMP-MAP VERBS (proposal 43 N1).
 *
 * All four are ONE action type. A comp map is a small value, and every edit to
 * it is "replace the map" — so the honest verb set is four spellings of one
 * mutation, each carrying the WHOLE resulting map, with the inverse carrying
 * the whole previous one. That is exactly `set-automation-points`' shape
 * (proposal 37 P5) and for the same reason: a per-segment diff would need an
 * identity for a segment, and a segment's only identity is its position, which
 * is the thing being edited.
 *
 *   `set-comp-segment`     at, take, [xfade]  — add or REPLACE at a position
 *   `remove-comp-segment`  at                 — drop the segment at a position
 *   `move-comp-boundary`   at, to             — move one boundary
 *   `set-comp-xfade`       at, xfade          — the crossfade at one boundary
 *
 * `clip` addresses the column exactly as every other take verb does, through
 * `stakes::columnOfLink`, so BOTH shapes work (proposal 42).
 *
 * N1 has no consumer: the map is model state and nothing sounds different.
 */
class SCompMapAction : public SAction
{
public:
    enum class Op { SetSegment, RemoveSegment, MoveBoundary, SetXfade, SetWhole };

    SCompMapAction() {}
    SCompMapAction( Op op, const QString &clip, qint64 at, int take,
                    qint64 xfade, qint64 to );

    SApplyResult apply( SProject *project ) override;
    QString name() const override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /**
     * The whole-map form, used by every inverse and by `select-take`'s own
     * inverse. Not reachable from a script by name: a script edits a map one
     * boundary at a time, and a verb that took a whole map would need a
     * spelling for it in XML that nothing would ever hand-write.
     */
    static SCompMapAction *wholeMap( const QString &clip, const twCompMap &map );

private:
    Op op_ = Op::SetSegment;
    QString clip_;
    qint64 at_ = 0;
    int take_ = 0;
    qint64 xfade_ = 0;
    qint64 to_ = 0;
    twCompMap whole_;
};

#endif // _SCOMPMAPACTIONS_H_
