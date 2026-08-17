#ifndef _SAUTOMATIONLANE_H
#define _SAUTOMATIONLANE_H

#include "tw/core/twtypes.h"
#include "tw/events/twautomationcurve.h"

#include <QDomElement>
#include <QObject>
#include <QString>
#include <QTextStream>
#include <memory>
#include <mutex>
#include <vector>

/**
 * AUTOMATION LANES (proposal 37 P5, design §3.3 / D5).
 *
 * A lane is a plain owner-held QObject and NEVER an `SLink` child. That is not
 * an implementation shortcut, it is the persistence contract: the project
 * loader orders and resolves on `<SLink>` children only, so an inline
 * `<automation>` child of a known element is invisible to it and an OLDER build
 * simply ignores it (design §3.3, model/CONTRACT). A lane that were an SObject
 * would need an id, a link, a place in the load order and a policy for what
 * happens when its owner is dropped — for a breakpoint table that has no
 * independent existence.
 *
 * WHAT A LANE IS: a target (`ParamRef`), a mode, and a sorted, immutable point
 * table. Every mutation REPLACES the table and rebuilds a
 * `twAutomationCurve` SNAPSHOT (shared_ptr<const>), which is what the consuming
 * engine component is handed under its own mutex and reads once per page into a
 * local (THREADING rule 2). Nothing the engine holds is ever mutated in place.
 *
 * THE VALUE DOMAIN IS THE TARGET'S OWN (design §3.3):
 *
 *   self:Volume        dB, the fader's own unit and range (app/timeline/
 *                      sfadercurve.h: -96 .. +24). A Linear segment
 *                      interpolates linearly IN dB — which is the gain stage's
 *                      own space, and the only reading under which Trim's
 *                      "static value x curve" is the dB SUM the design asks
 *                      for. (Recorded as a P5 decision: the design's "dB-linear
 *                      in fader space" is read as "linear in dB, in the fader's
 *                      space", because twAutomationCurve interpolates the
 *                      STORED value and tw/mix may not include an app header.)
 *   self:Muted         0 = audible, 1 = muted. A STEP lane, and the ONE lane
 *                      that holds its DEFAULT (audible) before its first point
 *                      rather than that point's value: "muted from frame 0" is
 *                      what the structural mute says, and a lane drawn to mute
 *                      a track at 1 s must not silence everything before it.
 *   param:<id>         the plugin's HOST-FACING domain — native for CLAP/AU,
 *                      normalized [0,1] for VST3 (plugins inv. 26), i.e. the
 *                      same numbers `set-plugin-param` writes.
 *   cut:Gain           a LINEAR amplitude factor, 1.0 = unity. Linear rather
 *                      than dB so a clip envelope can reach EXACTLY zero, which
 *                      is what a fade-out is.
 *   cut:VelocityScale  a factor on note velocity, 1.0 = unchanged.
 *   cut:Transpose      semitones, 0 = unchanged.
 *
 * `Rate` / `Stretch` are deliberately NOT automatable: they change a clip's
 * duration, and a duration that varied with position is not a window.
 */

/// Design §3.4. Trim is the default (§11 decision 3): the static value stays
/// meaningful when a lane exists. Touch/Latch/Write are UI RECORDERS — P6 owns
/// the gesture; here they read exactly like Read.
enum class SAutomationMode {
    Off = 0,   // the lane is stored but not consumed
    Trim,      // the DEFAULT: static value x curve (for Volume: the dB SUM)
    Read,      // the curve alone; the static value is overridden
    Touch,
    Latch,
    Write
};

QString         sAutomationModeToString( SAutomationMode m );
SAutomationMode sAutomationModeFromString( const QString &s, bool *ok = nullptr );

/**
 * A parameter reference: which knob, in which space (design §3.3).
 * Spellings: `self:Volume`, `self:Muted`, `param:<id>`, `cut:Gain`,
 * `cut:VelocityScale`, `cut:Transpose`.
 */
struct SParamRef {
    enum class Space { Unknown = 0, Self, Param, Cut };

    Space          space   = Space::Unknown;
    QString        prop;                 // Self / Cut: the property name
    std::uint32_t  paramId = 0;          // Param only

    static SParamRef parse( const QString &target );
    QString toString() const;
    bool    isValid() const { return space != Space::Unknown; }

    /// The value a lane of this target reports where it has no points, and the
    /// identity the consumer falls back to when the lane is Off or absent.
    double defaultValue() const;
};

struct SAutomationPoint {
    offset_t     frame   = 0;      // the OWNER's time domain, whole frames
    double       value   = 0.0;
    twCurveShape shape   = twCurveShape::Linear;   // governs [this, next)
    double       tension = 0.0;                    // Exp only

    bool operator==( const SAutomationPoint &o ) const
    {
        return frame == o.frame && value == o.value && shape == o.shape
            && tension == o.tension;
    }
};

class SAutomationLane : public QObject
{
    Q_OBJECT
public:
    /// `target` is the ParamRef spelling; it is IMMUTABLE for the lane's life
    /// (a lane is identified by owner + target everywhere: mergeKey, the verbs,
    /// the XML).
    explicit SAutomationLane( const QString &target );
    virtual ~SAutomationLane();

    const QString   &target() const { return targetSpelling_; }
    const SParamRef &ref() const { return ref_; }

    SAutomationMode mode() const { return mode_; }
    /// Returns true when the mode actually changed.
    bool setMode( SAutomationMode m );

    /// Recovery name for a `param:` lane (design §3.3: "+ `name`"). Purely
    /// informational — resolution is always by id.
    const QString &paramName() const { return paramName_; }
    void setParamName( const QString &n ) { paramName_ = n; }

    /// The point table, sorted by frame. A COPY, deliberately: callers build
    /// the next table from it and hand it back through setPoints(), which is
    /// what keeps the live table immutable while a freeze reads its snapshot.
    std::vector<SAutomationPoint> points() const;
    int pointCount() const;

    /// Replace the whole table (sorted and de-duplicated by frame here, so no
    /// caller has to). Rebuilds the snapshot. Returns true when it changed.
    bool setPoints( std::vector<SAutomationPoint> pts );

    /// The value the MODEL reports at `frame` — the same number
    /// `assert-automation-value` asserts and the same one the snapshot yields.
    double valueAt( offset_t frame ) const;

    /// The snapshot handed to the engine. Null when the lane has no points or
    /// its mode is Off — "no curve" is the SCALAR path, which is what keeps
    /// every render without a lane byte-identical (P5 AC6).
    std::shared_ptr<const twAutomationCurve> snapshot() const;

    bool isEmpty() const { return pointCount() == 0; }

    // --- the affected range of an edit (D5: the invalidation is EXACT) -------
    //
    // The value on [prev, T) depends on the point at T only when prev's shape
    // INTERPOLATES; a Step segment's value is its left point's alone. That
    // distinction is the whole reason qxa.automation_edit_invalidates can
    // assert byte identity over the seconds either side of an edit.
    struct Range {
        offset_t start = 0;
        offset_t end   = 0;
        bool empty() const { return end <= start; }
        void unite( const Range &o );
    };
    /// The span whose rendered value can move when the point at (or nearest
    /// below) `frame` changes. `end` is INT64_MAX when nothing follows.
    Range rangeAround( offset_t frame ) const;
    /// The union of rangeAround() over every frame in the list.
    Range fullRange() const;

    // --- persistence (inline in the owner element) --------------------------
    //   <lane target='self:Volume' mode='trim' name='Cutoff'>
    //     <p t='0' v='-60' c='linear'/>
    //     <p t='192000' v='0' c='linear'/>
    //   </lane>
    void serialize( QTextStream &o ) const;
    /// Reads target/mode/name/points out of a `<lane>` element. Returns false
    /// when the element carries no parsable target.
    bool readFrom( const QDomElement &laneEl );

    static QString      shapeToString( twCurveShape c );
    static twCurveShape shapeFromString( const QString &s );

    /// Emit changed(). The RANGE is known to the verb, not to the lane, so the
    /// verb is what announces it — a lane cannot tell a point move from a
    /// wholesale replacement.
    void notifyChanged( offset_t start, offset_t end ) { emit changed( (qint64) start,
                                                                      (qint64) end ); }

signals:
    /// Emitted after a mutation, with the affected range in the OWNER's domain.
    /// Owners connect nothing — SObject calls onAutomationChanged() directly —
    /// but the UI (P6) needs a repaint hook that is not the invalidation path.
    void changed( qint64 start, qint64 end );

private:
    void rebuildSnapshot_nolock();

    mutable std::mutex            mutex_;
    QString                       targetSpelling_;
    SParamRef                     ref_;
    QString                       paramName_;
    SAutomationMode               mode_ = SAutomationMode::Trim;
    std::vector<SAutomationPoint> points_;
    std::shared_ptr<const twAutomationCurve> snapshot_;
};

#endif // _SAUTOMATIONLANE_H
