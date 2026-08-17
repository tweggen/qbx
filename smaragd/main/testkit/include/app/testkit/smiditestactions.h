#ifndef _SMIDITESTACTIONS_H_
#define _SMIDITESTACTIONS_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"

/**
 * `assert-midi-events` - what a clip, or a TRACK'S FEED, actually holds.
 *
 * Two scopes, because there are two things worth asserting and they are not
 * the same object:
 *
 *   scope="clip" (default, `clip=`)  - the cut's own frame-domain snapshot:
 *       the notes it carries, transposed / velocity-scaled / channel-mapped as
 *       the clip says. This is what the edit verbs move.
 *
 *   scope="feed" (`trackPath=`)      - `STrack::eventFeed()->collect()` over
 *       [startFrame, startFrame+frameCount): the merge of the track's own clip
 *       set with every child track that bubbles events up (design 3.2.1). It
 *       is the ONLY way to see mute / solo / midiRouting, and it is what an
 *       instrument will read in P3b.
 *
 * A `collect` is also the only place a SYNTHESISED note-off exists: it is not
 * in any table, it is invented at the clip end for a note still held there
 * (events/CONTRACT inv. 8). `kind="noteoff-synth"` asserts exactly those, which
 * is how the non-destructive split is gated - the head's window end releases a
 * straddling note, and the tail never re-attacks it.
 */
class SAssertMidiEventsAction : public SAction
{
public:
    SAssertMidiEventsAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-midi-events" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  scope_ = QStringLiteral( "clip" );
    QString  clip_;            // index path (scope=clip)
    QString  trackPath_;       // index path (scope=feed)
    int      take_ = -1;

    QString  kind_;            // "" = any; note|noteon|noteoff|noteoff-synth|cc|...
    qint64   at_ = -1;         // frames; -1 = any
    qint64   tolerance_ = 0;   // frames of slack around `at`
    int      key_ = -1;
    double   velocity_ = -1.0;
    double   velocityTolerance_ = 0.5;
    int      channel_ = -1;

    int      count_ = -1;      // -1 = not checked
    int      minCount_ = -1;
    int      maxCount_ = -1;

    qint64   startFrame_ = 0;  // the collect window (scope=feed)
    qint64   frameCount_ = -1; // < 0 = the whole arrangement

    QString  contains_;        // substring of the describe() dump
};

/**
 * `assert-midi-file` - counts and shape of a `.mid` on disk.
 *
 * The companion of `assert-file-identical`, not a replacement: byte identity
 * is the gate for a file this writer AUTHORED, while this one is what a
 * FOREIGN file (or an export whose bytes may legitimately differ) can be held
 * to - track count, note count, the first tick, the division.
 */
class SAssertMidiFileAction : public SAction
{
public:
    SAssertMidiFileAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-midi-file" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString filename_;
    int     trackCount_ = -1;
    int     noteCount_ = -1;
    int     eventCount_ = -1;
    qint64  firstTick_ = -1;
    int     ppq_ = -1;
    int     format_ = -1;
};

#endif // _SMIDITESTACTIONS_H_
