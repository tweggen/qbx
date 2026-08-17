#ifndef _SASSERTCLIPWINDOWACTION_H_
#define _SASSERTCLIPWINDOWACTION_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"

/**
 * `assert-clip-window` - a clip's PLACEMENT and WINDOW, in timeline frames.
 *
 * Everything the arranger draws about a clip and nothing else: where it starts,
 * how long it is, how far into its content it begins, how long its repeating
 * segment is, and which timebase its placement is anchored to. It reads the
 * `SClipWindow` interface, so it works for an audio cut, an event clip and any
 * later window type without knowing which it has (CLIP_MODEL: the verbs
 * address the interface, not the class).
 *
 * It exists because `set-tempo` has to be gated on exactly this: a
 * beats-timebase MIDI clip MOVES and an audio clip does NOT, and until now the
 * only way to see a clip's geometry from a script was to render it and listen.
 * A rendered assertion cannot separate "the clip moved" from "the clip moved
 * and its content moved the other way".
 *
 * Every bound is optional: -1 (or "" for `timebase`) means "not checked", so a
 * case asserts the one thing it is about.
 */
class SAssertClipWindowAction : public SAction
{
public:
    SAssertClipWindowAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-clip-window" ); }
    QStringList knownAttributes() const override
    { return { "clip", "startTime", "duration", "loopLength", "startOffset",
               "timebase", "take" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString clip_;
    qint64  startTime_ = -1;
    qint64  duration_ = -1;
    qint64  loopLength_ = -1;
    qint64  startOffset_ = -1;
    QString timebase_;
    int     take_ = -1;
};

#endif // _SASSERTCLIPWINDOWACTION_H_
