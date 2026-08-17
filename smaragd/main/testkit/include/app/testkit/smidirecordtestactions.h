#ifndef _SMIDIRECORDTESTACTIONS_H_
#define _SMIDIRECORDTESTACTIONS_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"

/**
 * `assert-midi-recorded` - the MIDI placement gate (proposal 21 L4).
 *
 *     <assert-midi-recorded trackPath="0"
 *                           clips="1" takes="-1" minTakes="-1"
 *                           passes="-1" minPasses="-1"
 *                           notes="-1" minNotes="-1" events="-1"
 *                           startFrame="-1" startTolerance="1024"
 *                           durationFrames="-1" durationTolerance="4096"
 *                           mode="" quantize="" contains=""/>
 *
 * It is the sibling of `assert-recorded-clip` and it deliberately does NOT
 * assert WHERE a note landed: that is `assert-midi-events`' job, over the
 * clip's own frame-domain snapshot, and it is a much better instrument for it
 * (per note, with a tolerance, on the real snapshot every consumer reads). What
 * this verb asserts is the SHAPE of what the recorder committed - how many
 * columns, how many takes on them, how many passes the recorder says it
 * committed and how many notes went in - plus the recorder's own state string.
 *
 * The counts that come from `SMidiRecorder` and the counts that come from the
 * MODEL are BOTH checked, which is the point: a recorder that reports three
 * passes while the lane holds one take is exactly the failure "one take per
 * pass" is a claim about, and neither number alone can see it.
 *
 * A PASS COUNT IS A WALL-CLOCK QUANTITY when cycling - captured material over
 * loop length - so a loop case asserts `minPasses` and lets the verb check the
 * take/pass IDENTITY unconditionally, exactly as `assert-recorded-clip` does
 * for audio. `notes` is not wall-clock (a replayed performance delivers what it
 * delivers), so it can be exact.
 */
class SAssertMidiRecordedAction : public SAction
{
public:
    SAssertMidiRecordedAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-midi-recorded" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    static constexpr qint64 kUnset = -1;

    QList<int> trackPath_;
    qint64 clips_             = kUnset;
    qint64 takes_             = kUnset;
    qint64 minTakes_          = kUnset;
    qint64 passes_            = kUnset;
    qint64 minPasses_         = kUnset;
    qint64 notes_             = kUnset;
    qint64 minNotes_          = kUnset;
    qint64 events_            = kUnset;
    qint64 startFrame_        = kUnset;
    qint64 startTolerance_    = 1024;
    qint64 durationFrames_    = kUnset;
    qint64 durationTolerance_ = 4096;
    QString mode_;
    QString quantize_;
    QString contains_;
};

#endif // _SMIDIRECORDTESTACTIONS_H_
