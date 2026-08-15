#ifndef _SEVENTEDITORVIEW_H_
#define _SEVENTEDITORVIEW_H_

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <vector>

#include "app/objects/midi/smidisequence.h"
#include "tw/core/twfraction.h"
#include "tw/core/twtypes.h"

class SEventTimeAxis;
class SLink;
class SMidiCut;
class SProject;

/**
 * SEventEditorView - the abstract face every event editor presents to the dock
 * (proposal 36 6.2).
 *
 * The dock owns the toolbar, the ruler and the CLIP PATH; a view owns nothing
 * but a way to draw a snapshot of that clip and a way to commit an edit. That
 * split is what lets a tracker grid, a score and a drum grid be added later as
 * pure additions: they are bound to the same clip snapshot, the same time axis
 * and the same batch verbs, and the dock never learns which one it is showing.
 *
 * THE RULES A SUBCLASS INHERITS (and must not re-spell):
 *
 *  1. NEVER cache an SLink* or an SMidiCut*. A view holds an index PATH and
 *     re-resolves it on every use (timeline invariant 8) - any action can
 *     destroy a selected link, and this codebase has been burned by exactly
 *     that use-after-destruction.
 *  2. EVERY edit commits through a P1 verb, one action per GESTURE (timeline
 *     invariant 3). A drag of N notes is one `set-notes`, not N `add-note`s;
 *     the live drag paints a preview out of `drag_` and touches no model at
 *     all, and the release REVERTS the preview and submits the action.
 *  3. Positions cross the tick<->frame boundary only through the project's
 *     twTempoMap and the clip's own rate (POSITION_DOMAINS rule 7). The
 *     helpers below are the only place this module does that arithmetic.
 */
class SEventEditorView : public QWidget
{
    Q_OBJECT
public:
    explicit SEventEditorView( QWidget *parent = nullptr );
    ~SEventEditorView() override;

    /** Registry key ("pianoroll"), and the label the kind switch shows. */
    virtual QString kind() const = 0;

    /** The clip this view edits, as an index path from the root mixer. */
    void setClip( const QList<int> &clipPath );
    const QList<int> &clipPath() const { return clipPath_; }

    /** The shared axis. Not owned; the dock outlives every view. */
    void setTimeAxis( SEventTimeAxis *axis );
    SEventTimeAxis *timeAxis() const { return axis_; }

    /** Grid division as the toolbar spells it ("1/16", "1/8t"). */
    void setGrid( const QString &grid );
    const QString &grid() const { return grid_; }

    /** Editing tool. Draw inserts, Erase removes, Select moves/resizes. */
    enum class Tool { Select, Draw, Erase };
    void setTool( Tool t ) { tool_ = t; }
    Tool tool() const { return tool_; }

    /** Re-resolve the clip and repaint. Cheap, idempotent, re-entrancy safe. */
    virtual void refresh();

    /**
     * The test face (`assert-event-editor`). The BASE fields, in this order:
     *
     *     kind=<kind>|notes=<n>|grid=<div>|linked=<0/1>|empty=<0/1>|selection=<n>
     *
     * The order is load-bearing: the qxa `contains=` matches a CONTIGUOUS
     * substring, so a case asserts "notes=1|grid=1/16|linked=1" as one string.
     * A subclass appends its own fields AFTER these.
     */
    virtual QString describe() const;

    /** Extra fields a subclass appends to describe(). Empty by default. */
    virtual QString describeExtra() const { return QString(); }

    /** How many notes the view currently shows, and how many are selected. */
    virtual int noteCount() const = 0;
    virtual int selectionCount() const = 0;

protected:
    /**
     * A resolved clip. Built fresh at every use and NEVER stored in a member
     * (rule 1 above). `valid()` is the only gate a caller needs.
     */
    struct Resolved {
        SProject      *project = nullptr;
        SLink         *link    = nullptr;   // the placement (timeline start)
        SMidiCut      *cut     = nullptr;   // the window
        SMidiSequence *seq     = nullptr;   // the content
        bool valid() const { return project && link && cut && seq; }
    };
    Resolved resolve() const;

    // --- the tick <-> frame <-> pixel chain (rule 3) ----------------------
    //
    // A content tick is the domain `add-note` / `set-notes` speak; a timeline
    // frame is the domain the axis speaks. The window's anchor and rate sit
    // between them, exactly as SMidiCut::timelineToSourceExact spells it.
    offset_t frameOfTick( const Resolved &r, qint64 tick ) const;
    qint64   tickOfFrame( const Resolved &r, offset_t frame ) const;
    int      xOfTick( const Resolved &r, qint64 tick ) const;
    qint64   tickOfX( const Resolved &r, int x ) const;

    /** The grid division in CONTENT ticks (0 when it does not parse). */
    qint64 gridTicks( const Resolved &r ) const;
    /** Snap a content tick to the current grid. */
    qint64 snapTick( const Resolved &r, qint64 tick ) const;

    /** The clip's notes, sorted, straight out of the content table. */
    std::vector<SEvent> notesOf( const Resolved &r ) const;

    /**
     * Commit an absolute new NOTE state as ONE `set-notes` (rule 2). Non-note
     * events are untouched by the verb, so drawing never destroys CCs.
     */
    void commitNotes( const Resolved &r, const std::vector<SEvent> &notes );

    QList<int>      clipPath_;
    SEventTimeAxis *axis_ = nullptr;
    QString         grid_ = QStringLiteral( "1/16" );
    Tool            tool_ = Tool::Select;
};

/**
 * The KIND REGISTRY (proposal 36 6.2). A view kind registers itself from a
 * static initializer in its own .cpp, exactly as the actions and the loader
 * factories do - which is why `app_ui` must stay an OBJECT library.
 *
 * The dock therefore has no list of editors in it: adding a tracker grid is a
 * new file plus one static initializer, and the kind switch grows an entry.
 */
class SEventEditorRegistry
{
public:
    using Factory = std::function<SEventEditorView *( QWidget * )>;

    static SEventEditorRegistry &instance();

    /** `kind` is the registry key AND the `assert-event-editor kind=` value. */
    void registerKind( const QString &kind, const QString &label, Factory f );

    /** Registration order, which is the order the kind switch lists them. */
    QStringList kinds() const;
    QString labelFor( const QString &kind ) const;
    /** Null for an unknown kind. */
    SEventEditorView *create( const QString &kind, QWidget *parent ) const;

private:
    SEventEditorRegistry() = default;
    struct Entry { QString kind; QString label; Factory factory; };
    std::vector<Entry> entries_;
};

#endif // _SEVENTEDITORVIEW_H_
