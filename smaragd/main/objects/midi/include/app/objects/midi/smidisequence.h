#ifndef _SMIDISEQUENCE_H_
#define _SMIDISEQUENCE_H_

#include <QByteArray>
#include <QMap>
#include <QString>
#include <memory>
#include <vector>

#include "app/model/sobject.h"
#include "tw/core/twfraction.h"
#include "tw/events/twevent.h"
#include "tw/events/twautomationcurve.h"
#include "tw/events/tweventseq.h"

class QDomElement;
class SObjectRenderer;
class SMidiSequenceRendererInline;
class SProject;
class SProjectLoader;
class twComponent;

/**
 * SEvent - one event as the MODEL stores and persists it (proposal 37 3.1).
 *
 * It is deliberately the same vocabulary as the engine's `twEvent` (kind,
 * channel, key, value/value2, duration, paramId) rather than a second one:
 * events/CONTRACT invariant 1 says there is ONE event type, and a model record
 * that renamed the fields would need a translation table that could disagree
 * with itself. What it adds is what a value type can hold and an arena-indexed
 * struct cannot - the payload as a QByteArray, the text, and the verbatim
 * attribute map of an element whose `k` this build does not know.
 *
 * TIMES ARE MUSICAL TICKS at the owning sequence's ppq. Nothing here is ever
 * frames: the conversion happens once, inside SMidiCut (POSITION_DOMAINS rule
 * 7).
 */
struct SEvent {
    qint64      t = 0;            // ticks, sequence-relative
    qint64      dur = 0;          // NoteOn only, ticks (notes are stored WITH length)
    twEventKind kind = twEventKind::NoteOn;
    int         channel = -1;     // 0-15, -1 = n/a
    int         key = -1;         // note number, -1 = n/a
    quint32     paramId = 0;      // CC number | SMF meta type | sysex status byte
    double      value = 0.0;      // velocity / CC value / bend / ...
    double      value2 = 0.0;     // release velocity / tuning cents / timesig den
    QString     text;             // lyric / marker / chord symbol / articulation
    QByteArray  blob;             // sysex bytes / unknown meta payload, PRESERVED
    bool        muted = false;

    // The attributes of an `<e>` whose `k` this build did not recognise, kept
    // so a file from a newer build round-trips VERBATIM. Empty for every event
    // we understand. `rawKind` is that element's `k` string.
    QString                rawKind;
    QMap<QString, QString> extra;

    // Sort key: time first, then a total order so a write is byte-stable
    // (proposal 32 diff stability) and two equal tables compare equal.
    bool operator<( const SEvent &o ) const;

    /** Where this event's span ends, in ticks (a note ends after its length). */
    qint64 endTick() const
    { return t + ( kind == twEventKind::NoteOn ? dur : 0 ); }
};

/**
 * SMidiSequence - the EVENT CONTENT object (the SPlainWave analogue).
 *
 * Holds a sorted `std::vector<SEvent>` in MUSICAL TICKS at PPQ 960 (D2: the
 * domain every reference DAW stores MIDI in, and the one that makes a tempo
 * change keep the notes where the musician put them). It is persisted INLINE
 * - `<SMidiSequence ...><events count='N'><e .../></events></SMidiSequence>` -
 * because note data must never be able to go missing the way a sample file
 * can; an imported `.mid` is materialised on the first save.
 *
 * It produces no audio. `getRandomSource()` is null and `getRootComponent()`
 * returns a private SILENCE component: a null component would make
 * `twView::getComponent()` warn once per freeze for the lifetime of the
 * project (facts M5/F12), which is noise, not information. Nothing routes an
 * event clip through a bus mixer anyway - STrack puts it in its event clip set
 * (3.2).
 *
 * Threading: the event table is IMMUTABLE and swapped whole. Every edit
 * rebuilds `tickSnapshot()` under mutex(); a consumer that already holds one
 * keeps a coherent table for its whole use (THREADING rule 2,
 * events/CONTRACT inv. 5).
 */
class SMidiSequence
    : public SObject
{
    Q_OBJECT
public:
    /** Where the material came from. Informational; persisted for round-trip. */
    enum class Origin { Drawn = 0, Smf = 1, Recorded = 2 };

    /** The house resolution (D2). The tempo map carries the same number. */
    static constexpr int DEFAULT_PPQ = 960;

    explicit SMidiSequence( SProject *project );
    virtual ~SMidiSequence();

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader,
                                             QDomElement &element,
                                             SObject *parent );

    // --- content ---------------------------------------------------------

    int ppq() const { return ppq_; }
    Origin origin() const { return origin_; }
    void setOrigin( Origin o ) { origin_ = o; }

    /** The whole table, sorted. UI/edit-path read. */
    std::vector<SEvent> events() const;
    int eventCount() const;

    /**
     * Replace the whole table (the ONE mutator - every verb is expressed as an
     * absolute new state, which is also what makes an inverse trivial). Sorts,
     * recomputes the length, republishes the snapshot, and emits
     * eventsChanged(0) so every window over this content re-derives and every
     * track invalidates from 0 onward.
     */
    void setEvents( std::vector<SEvent> events );

    /** Musical length. Explicit when set; otherwise the last event's end. */
    qint64 lengthTicks() const;
    void setLengthTicks( qint64 ticks );

    /**
     * The TICK-domain engine table, immutable. This is what `export-midi-file`
     * hands to twSmf and what SMidiCut re-expresses in frames. Never converted
     * here - the domain of a sequence's time belongs to its owner
     * (events/CONTRACT inv. 3).
     */
    std::shared_ptr<const twEventSeq> tickSnapshot() const;

    // --- SObject ---------------------------------------------------------

    std::shared_ptr<twComponent> getRootComponent() override;
    QWidget *getDetailEditWidget( QWidget *parent ) override;
    QWidget *getInlineEditWidget( QWidget *parent ) override;
    SObjectRenderer *getInlineRenderer() override;

    SContentKind contentKind() const override { return SContentKind::Event; }

    bool hasDuration() const override { return true; }
    length_t getDuration() const override;

    /**
     * Ticks are RATE-FREE, so the base class's `durationSec` migration - which
     * multiplies a stored seconds value by the project rate - must not touch
     * this object (D2). The length comes from the events and from
     * `lengthTicks`, both read in readPreChildrenAttributes.
     */
    void setDuration( length_t ) override {}

    int readPreChildrenAttributes( QDomElement &element ) override;
    int serialize( QTextStream &o ) override;

protected:
    int serializeSelfAttributes( QTextStream &o ) override;

private:
    // Callers hold mutex().
    void rebuild_nolock();

    std::vector<SEvent> events_;
    std::shared_ptr<const twEventSeq> tickSeq_;
    qint64 lengthTicks_ = 0;       // explicit length; 0 = derive from events
    qint64 contentEndTicks_ = 0;   // the events' own end
    int    ppq_ = DEFAULT_PPQ;
    Origin origin_ = Origin::Drawn;

    SMidiSequenceRendererInline *inlineRenderer_ = nullptr;
    std::shared_ptr<twComponent> cpSilence_;
};

// --- SEvent <-> XML / twEvent, the ONE translation ------------------------
namespace smidievents {

/** The XML spelling of a kind, or a hex meta type for an unknown one. */
QString kindToString( const SEvent &e );
/** Parse a `k` attribute; false when it is not a spelling we know. */
bool kindFromString( const QString &s, twEventKind &kind, quint32 &paramId );

/** Read one `<e .../>` element. Unknown attributes land in `extra`. */
SEvent readEvent( const QDomElement &e );
/** Write one `<e .../>` element (defaults omitted, `extra` re-emitted). */
void writeEvent( QTextStream &o, const SEvent &e );

/**
 * Build an engine table out of model events. `scale` re-expresses every time
 * and duration (identity for the tick domain; the tempo-map factor for the
 * frame domain), `transpose`/`velocityScale`/`channelOverride` are SMidiCut's
 * per-clip modifiers, applied here so there is exactly one place that does it.
 */
/**
 * `transCurve` / `velCurve` are the clip's `cut:Transpose` / `cut:VelocityScale`
 * AUTOMATION lanes (proposal 37 P5), in the clip's own frame domain — which is
 * exactly the domain each event's converted `time` is already in. They compose
 * with the static modifiers the way Trim always does: the transpose lane ADDS
 * semitones, the velocity lane MULTIPLIES. Null is the untouched path.
 */
std::shared_ptr<const twEventSeq> buildSeq(
    const std::vector<SEvent> &events, const Fraction &scale,
    int transpose = 0, double velocityScale = 1.0, int channelOverride = -1,
    const twAutomationCurve *transCurve = nullptr,
    const twAutomationCurve *velCurve = nullptr );

}  // namespace smidievents

#endif // _SMIDISEQUENCE_H_
