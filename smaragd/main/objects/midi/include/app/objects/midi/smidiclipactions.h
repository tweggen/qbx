#ifndef _SMIDICLIPACTIONS_H_
#define _SMIDICLIPACTIONS_H_

#include <QList>
#include <QString>
#include <vector>

#include "app/actions/saction.h"
#include "app/objects/midi/smidisequence.h"
#include "tw/core/twfraction.h"

class SRemoveMidiClipAction;

/**
 * `insert-midi-clip` - sequence + window + placement, in one step.
 *
 * `duration` is in TICKS and 0 means one bar of the project's time signature.
 * The link is born with `timebase=beats` (D2, via SLink::defaultTimebaseFor),
 * so the clip stays at its bar across a tempo edit without anyone asking.
 */
class SInsertMidiClipAction : public SAction
{
public:
    SInsertMidiClipAction() = default;
    SInsertMidiClipAction( const QList<int> &trackPath, offset_t timePos,
                           qint64 durationTicks, const QString &name,
                           std::vector<SEvent> events = {} );

    QString name() const override { return QStringLiteral( "insert-midi-clip" ); }
    QStringList knownAttributes() const override
    { return { "trackPath", "timePos", "duration", "name" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int>          trackPath_;
    offset_t            timePos_ = 0;
    qint64              durationTicks_ = 0;
    QString             clipName_;
    // Carried by the INVERSE of a removal so undo/redo restores the notes too,
    // not just an empty clip (a lost take is not an undo).
    std::vector<SEvent> events_;
};

/** insert-midi-clip's inverse. Not registered as a verb - created live. */
class SRemoveMidiClipAction : public SAction
{
public:
    SRemoveMidiClipAction( const QList<int> &clipPath,
                           const QList<int> &trackPath, offset_t timePos,
                           qint64 durationTicks, const QString &name,
                           std::vector<SEvent> events );

    QString name() const override { return QStringLiteral( "remove-midi-clip" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int>          clipPath_;
    QList<int>          trackPath_;
    offset_t            timePos_ = 0;
    qint64              durationTicks_ = 0;
    QString             clipName_;
    std::vector<SEvent> events_;
};

/**
 * `set-midi-cut` - the per-clip modifiers, ABSOLUTE (like `set-pitch`).
 *
 * The WINDOW is deliberately not here: length, slip, loop and rate go through
 * the generalized `resize-clip` (frames in, converted once inside the window),
 * so there is no second window verb to keep in step.
 */
class SSetMidiCutAction : public SAction
{
public:
    SSetMidiCutAction() = default;
    SSetMidiCutAction( const QList<int> &clipPath, int transpose,
                       double velocityScale, int channel, int take,
                       bool broadcast );

    QString name() const override { return QStringLiteral( "set-midi-cut" ); }
    QStringList knownAttributes() const override
    { return { "clip", "transpose", "velocityScale", "channel", "take",
               "broadcast" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    int        transpose_ = 0;
    double     velocityScale_ = 1.0;
    int        channel_ = -1;
    int        take_ = -1;
    bool       broadcast_ = true;
};

/**
 * `set-tempo` - THE ONLY tempo write (D2).
 *
 * It writes the project's twTempoMap and then RE-DERIVES `startTime` for every
 * `timebase=beats` link in the project - nested containers and assets included
 * - so a MIDI clip at bar 5 stays at bar 5 while audio does not move. The
 * ticks themselves are never touched, which is what stops repeated tempo edits
 * from drifting: a frames-only rescale loses a frame per edit.
 *
 * Being an action is also what keeps undo exact by LIFO: an inverse captured
 * in frames is always re-applied at the tempo it was captured under, because
 * nothing outside the action system writes the map after P1.
 */
class SSetTempoAction : public SAction
{
public:
    SSetTempoAction() = default;
    explicit SSetTempoAction( double bpm );

    QString name() const override { return QStringLiteral( "set-tempo" ); }
    QStringList knownAttributes() const override { return { "bpm" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    // A tempo spin box drag is one undo step.
    QString mergeKey() const override { return QStringLiteral( "set-tempo" ); }
    bool mergeWith( const SAction *later ) override;

private:
    double bpm_ = 120.0;
};

/** `set-link-timebase` - pin a placement to time, or to the beat. ABSOLUTE. */
class SSetLinkTimebaseAction : public SAction
{
public:
    SSetLinkTimebaseAction() = default;
    SSetLinkTimebaseAction( const QList<int> &clipPath, const QString &timebase );

    QString name() const override
    { return QStringLiteral( "set-link-timebase" ); }
    QStringList knownAttributes() const override { return { "clip", "timebase" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    QString    timebase_ = QStringLiteral( "beats" );
};

#endif // _SMIDICLIPACTIONS_H_
