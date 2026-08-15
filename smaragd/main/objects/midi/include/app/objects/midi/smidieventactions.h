#ifndef _SMIDIEVENTACTIONS_H_
#define _SMIDIEVENTACTIONS_H_

#include <QList>
#include <QString>
#include <vector>

#include "app/actions/saction.h"
#include "app/objects/midi/smidisequence.h"

/**
 * The event-content verbs (proposal 36 3.4).
 *
 * ONE mutator sits under all of them - `SMidiSequence::setEvents`, an absolute
 * new table - so every inverse is the same thing: a `set-events` carrying the
 * previous table verbatim. That is exactly what makes a piano-roll drag of N
 * notes one undo step and a quantize its own inverse, and it is why an event
 * edit can never leave the table in a state no verb could have produced.
 *
 * Split is NOT among them: an event clip is split by narrowing its WINDOW
 * (`split-clip` through `SClipWindow`), which never touches the shared content
 * (D3 - a content-editing `split-notes-at` is a later verb).
 */

/** `set-events` - replace the whole table. The universal inverse. */
class SSetEventsAction : public SAction
{
public:
    SSetEventsAction() = default;
    SSetEventsAction( const QList<int> &clipPath, std::vector<SEvent> events,
                      int take = -1 );

    QString name() const override { return QStringLiteral( "set-events" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "clip" ), QStringLiteral( "take" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int>          clipPath_;
    std::vector<SEvent> events_;
    int                 take_ = -1;
};

/** `add-note` - one note, addressed by (tick, key, channel). */
class SAddNoteAction : public SAction
{
public:
    SAddNoteAction() = default;
    SAddNoteAction( const QList<int> &clipPath, qint64 tick, qint64 dur,
                    int key, double velocity, int channel,
                    double releaseVelocity, int take, bool broadcast );

    QString name() const override { return QStringLiteral( "add-note" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    qint64 tick_ = 0;
    qint64 dur_ = 480;
    int    key_ = 60;
    double velocity_ = 100.0;
    int    channel_ = 0;
    double releaseVelocity_ = 64.0;
    int    take_ = -1;
    bool   broadcast_ = true;
};

/** `remove-note` - the note at (tick, key, channel). */
class SRemoveNoteAction : public SAction
{
public:
    SRemoveNoteAction() = default;
    SRemoveNoteAction( const QList<int> &clipPath, qint64 tick, int key,
                       int channel, int take, bool broadcast );

    QString name() const override { return QStringLiteral( "remove-note" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    qint64 tick_ = 0;
    int    key_ = -1;
    int    channel_ = -1;
    int    take_ = -1;
    bool   broadcast_ = true;
};

/**
 * `set-notes` - THE batch verb. The `<n .../>` children are the absolute new
 * NOTE state of the clip; every non-note event survives untouched, so drawing
 * notes never destroys the CCs underneath them.
 */
class SSetNotesAction : public SAction
{
public:
    SSetNotesAction() = default;
    SSetNotesAction( const QList<int> &clipPath, std::vector<SEvent> notes,
                     int take, bool broadcast );

    QString name() const override { return QStringLiteral( "set-notes" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    // A piano-roll drag is one undo step: consecutive edits of the same
    // selection on the same clip coalesce before the engine sees either.
    QString mergeKey() const override;
    bool mergeWith( const SAction *later ) override;

private:
    QList<int>          clipPath_;
    std::vector<SEvent> notes_;
    int                 take_ = -1;
    bool                broadcast_ = true;
};

/** `add-event` - one non-note event (CC, bend, metadata, sysex). */
class SAddEventAction : public SAction
{
public:
    SAddEventAction() = default;
    SAddEventAction( const QList<int> &clipPath, const SEvent &event, int take );

    QString name() const override { return QStringLiteral( "add-event" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    SEvent     event_;
    int        take_ = -1;
};

/** `remove-event` - every event matching kind + tick (+ channel, + p). */
class SRemoveEventAction : public SAction
{
public:
    SRemoveEventAction() = default;
    SRemoveEventAction( const QList<int> &clipPath, const QString &kind,
                        qint64 tick, int channel, int paramId, int take );

    QString name() const override { return QStringLiteral( "remove-event" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    QString    kind_;
    qint64     tick_ = 0;
    int        channel_ = -1;
    int        paramId_ = -1;
    int        take_ = -1;
};

/**
 * `quantize-notes` - a `set-notes` composition, so its inverse is the previous
 * state like every other content edit. `strength` interpolates towards the
 * grid (1 = snap, 0 = no-op); `swing` displaces every odd grid slot by that
 * fraction of the grid.
 */
class SQuantizeNotesAction : public SAction
{
public:
    SQuantizeNotesAction() = default;
    SQuantizeNotesAction( const QList<int> &clipPath, const QString &grid,
                          double strength, double swing, int take,
                          bool broadcast );

    QString name() const override { return QStringLiteral( "quantize-notes" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /** "1/16", "1/8t", "1/4" -> ticks at `ppq`. 0 when unparseable. */
    static qint64 gridTicks( const QString &grid, int ppq );

private:
    QList<int> clipPath_;
    QString    grid_ = QStringLiteral( "1/16" );
    double     strength_ = 1.0;
    double     swing_ = 0.0;
    int        take_ = -1;
    bool       broadcast_ = true;
};

#endif // _SMIDIEVENTACTIONS_H_
