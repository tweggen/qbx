#ifndef _SRECORDPLANNER_H_
#define _SRECORDPLANNER_H_

#include "app/model/sobject.h"
#include "tw/events/twcompmap.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

class SObject;
class SCompositeAction;

/**
 * HOW A RECORDING LANDS ON A LANE THAT ALREADY HAS MATERIAL (plan 53 §5.1).
 *
 * One `Planner` per record mode. `place-recording` is a THIN VERB from here
 * on: it reads the lane once into a `LaneSnapshot`, picks the planner for the
 * mode it was handed, and applies whatever the planner appended to a
 * composite. The verb itself decides nothing.
 *
 * THREE RULES CARRY THE DESIGN, and every planner is held to them:
 *
 *  1. A PLANNER EMITS EXISTING VERBS and never mutates the model. Its undo is
 *     therefore the composite's inverse chain, and a planner is testable by
 *     the verbs it emits rather than only through the model it leaves behind.
 *
 *  2. `snapshot()` IS THE ONE READER OF LANE STATE, and it CLASSIFIES without
 *     mutating. No planner may look at the lane again: a planner that read
 *     the live tree halfway through its own plan would be reading a tree its
 *     own earlier verbs have not been applied to yet.
 *
 *  3. INDEX BOOKKEEPING IS THE PLANNER'S, IN APPLY ORDER (plan 53 G10). Lane
 *     child indices are INSERTION order, not time order, and the verbs a
 *     planner emits change them: `place-clip` and `split-clip` APPEND,
 *     `add-take` preserves its operand's index, `merge-columns` keeps its
 *     FIRST operand's index and REMOVES the others. So a planner that needs a
 *     path after it has emitted anything structural asks `ChildList`, never
 *     the snapshot. The old planner's comment "paths are computed NOW and stay
 *     valid through the plan" was true only because it emitted nothing but
 *     appends; it is false the moment a split or a merge is in the plan.
 *
 * WHY HERE. `objects/cut` is where `place-recording`, `add-take`,
 * `split-clip`, `resize-clip`, `remove-sample` and the column helpers live,
 * and `tools/check_layering.py` gives it the edges it needs for all of them.
 * It deliberately does NOT reach `objects/track`, which is why a planner is
 * handed its mode already resolved (`srecmode::resolveMode`, called by the
 * recorder) rather than resolving one itself.
 */
namespace srecplan {

/**
 * The four modes of plan 53 §3. The strings are `SProjectProps`' spelling,
 * which is what travels in a project file and in `place-recording mode=`.
 *
 * `Takes` is the default, and since P2 (QBX-180) it is MODE A -- the union
 * column of plan 53 §3.1, not the planner every build before plan 53 had.
 * `LegacyTakes` is gone; this is the row the whole plan was for.
 */
enum class Mode { Takes, TakesSplit, Overdub, Replace };

/**
 * `""`, and any value no planner knows, read as `Takes`.
 *
 * That is deliberately the same answer `SPlaceRecordingAction::readXml` gives
 * an unknown `mode=`: empty means "whatever this build would do anyway", and
 * "whatever this build would do anyway" is the legacy planner. A typo in a
 * `.qxa` therefore cannot reach a planner nobody named — but it also cannot
 * be told apart from no attribute at all, which is why `readXml` validates
 * the spelling at the door and the `.qxa` runner's attribute audit matters.
 */
Mode    modeFromString( const QString &mode );
QString modeToString( Mode mode );

/**
 * ONE lane child, read once at plan time.
 *
 * `index` is the child's index on the lane AT SNAPSHOT TIME — the path's last
 * step. After the plan has emitted anything that appends or removes, the
 * child's CURRENT index comes from `ChildList`, not from here.
 */
struct Child {
    /** What the child IS, as far as a recording plan is concerned. */
    enum Kind {
        Plain,          ///< a plain windowed clip (an `SCut`/`SMidiCut`)
        Column,         ///< a DIRECT take column with no comp map
        MappedColumn,   ///< a direct take column that carries a comp map
        WrappedColumn,  ///< `SLink -> SCut -> STakeStack` (Kw); see below
        OtherKind,      ///< a clip of the OTHER content kind (plan 53 B2, R0)
        Lane            ///< a sub-track; never a placement
    };

    int          index = -1;
    offset_t     start = 0;
    length_t     dur   = 0;
    Kind         kind  = Plain;
    /**
     * What the child's own material is.
     *
     * `kind` cannot carry this, because `OtherKind` is not a property of the
     * CHILD: an event clip is other-kind to an audio recording and is the
     * whole point of a MIDI one. So the classification records what the child
     * IS and the plan decides what that makes it for THIS pass
     * (`Context::kind`).
     */
    SContentKind contentKind = SContentKind::Audio;
    int      nTakes = 0;       ///< columns only
    int      activeTake = -1;  ///< columns only
    twCompMap map;             ///< columns only; empty = "ask activeTake"

    /**
     * Does the placement carry a clip WINDOW at all?
     *
     * Literally "the placement object is itself a window", which is false for
     * a DIRECT take column -- so what `merge-columns` accepts is
     * `isColumn() || hasWindow`, exactly its own
     * `if( !col && !SClipWindow::of( &obj ) )`. Mode A must know that BEFORE
     * it emits the verb: see `mixVolume` below for why a refusal from inside
     * the composite is not an option. No verb in the suite places raw content
     * on a lane, so this is true of everything a lane actually holds; it is
     * read rather than assumed because the cost of being wrong is a lost take.
     */
    bool     hasWindow = false;

    /**
     * THE PLACEMENT'S MIX PARAMETERS, read through the same
     * `SClipWindow::parametersOf` that `merge-columns` reads (so a DIRECT
     * column reports its ACTIVE take's, which is what the mix uses).
     *
     * WHY A PLANNER NEEDS THEM. `merge-columns` REFUSES operands that
     * disagree on volume or pan, or that carry anything POSITIONAL -- a
     * `cut:Gain` envelope or a clip fade -- because a merged column has ONE
     * set of clip parameters for its whole length (QBX-185, plan 53 §6.1).
     * That refusal is right for a user gesture and fatal for a recording:
     * `SCompositeAction::apply` rolls back every earlier child on the first
     * one that fails, so a mode A plan whose `merge-columns` refuses loses
     * the pass -- the one thing a recorder may not do. Mode A therefore
     * PRE-CHECKS the rule from the snapshot and degrades to a plain
     * placement, announced, instead of emitting a verb that will refuse.
     */
    double   mixVolume = 0.0;
    double   mixPan    = 0.0;
    bool     hasGainCurve = false;   ///< a non-empty `cut:Gain` lane
    bool     hasFade      = false;   ///< a clip fade (in or out)

    bool isColumn() const
    { return kind == Column || kind == MappedColumn || kind == WrappedColumn; }
    offset_t end() const { return start + (offset_t) dur; }
};

/**
 * The lane as the plan will read it, plus the path that addresses it.
 *
 * THE PATH IS PASSED IN RATHER THAN DERIVED, which is a deliberate deviation
 * from plan 53 §5.1's `snapshot( SObject *lane )`: an `SObject` cannot name
 * its own path, because the tree is `SObject -> SLink -> SObject` and only the
 * links point down. That is the same fact that makes `srecmode::resolveMode`
 * and `ssolo::underSoloedAncestorRec` walk DOWNWARD from the root. The caller
 * already holds the path — it is how it found the lane — so asking for it
 * costs nothing and inventing a second downward search would cost a walk of
 * the whole tree per recording.
 */
struct LaneSnapshot {
    QList<int>   lanePath;
    QList<Child> children;
    /**
     * The lane's TOTAL child count, including the children `snapshot()` left
     * out of `children`.
     *
     * IT IS NOT `children.size()`, and the difference is load-bearing.
     * `snapshot()` skips a live recording and a child with no duration, so a
     * `Child`'s `index` is its REAL index on the lane and the list may have
     * gaps. `ChildList` models the index SPACE, not the classified subset: a
     * simulation built from `children.size()` would map every index above a
     * gap one too low, and a plan computing a path from it would address the
     * WRONG child SUCCESSFULLY — silent corruption rather than a failed
     * action, which is the whole hazard `ChildList` exists to remove.
     */
    int          laneChildCount = 0;
};

/**
 * What the recorder hands over for ONE segment of ONE pass, AFTER the
 * pass-wide snap of plan 53 §3.5.
 *
 * `a`/`b` are the placed span in PROJECT frames, half-open. `srcOffset` is
 * SIGNED: negative means leading silence, which is legal and is what lets a
 * pass that began mid-cycle sit inside a take spanning the whole cycle
 * (proposal 23).
 */
struct Span {
    QString  filePath;
    offset_t a = 0;
    offset_t b = 0;
    offset_t srcOffset = 0;
    /**
     * THE TAKE'S EXTENT, when the recorder knows it is WIDER than the
     * material. `0` = `[a, b)`, the material's own span.
     *
     * THIS REPLACES THE `fromCycle` FLAG QBX-171 LEFT HERE UNREAD, and the
     * reason is plan 53 §3.1 step 5: "for a cycle span the take extent is the
     * WHOLE cycle". A boolean cannot say where the cycle ENDS, and `b` does
     * not: `place-recording` clamps the span to the material
     * (`min( length, waveDur - srcOffset )`) while the recorder's cycle branch
     * always asks for the whole `loopLen`, so the LAST pass of essentially
     * every cycle recording has `b < cycleEnd`. Giving that pass the
     * whole-cycle extent is what makes it an ALTERNATIVE FOR THE SAME BARS as
     * the full passes beside it -- and what lets `select-take` isolate it,
     * since A0b made the click extent-aware. Recorded as a plan correction.
     *
     * X8 ("no recorded take beyond the cycle") is the other direction and is
     * free: the extent is a maximum, never a minimum.
     */
    length_t extent = 0;

    length_t length() const { return b > a ? (length_t)( b - a ) : (length_t) 0; }
    /** Where the take's extent ends: `b`, or further when `extent` says so. */
    offset_t extentEnd() const
    {
        const offset_t e = a + (offset_t) extent;
        return e > b ? e : b;
    }
};

/**
 * Per-lane context for one pass.
 *
 * `armed` is false for an un-armed member of an armed lane's edit group.
 * **Nothing enumerates them before P4** (plan 53 §8, review 2 M5): every
 * planner, mode A included, treats `armed == false` as nothing to do. P4 is
 * where the takes planners grow the structure-only plan of §3.4.
 */
struct Context {
    Mode         mode  = Mode::Takes;
    bool         armed = true;
    SContentKind kind  = SContentKind::Audio;
};

/**
 * THE PLANNER'S MODEL OF THE LANE'S CHILD LIST, as its own emitted verbs will
 * leave it (rule 3 above).
 *
 * A slot is either an ORIGINAL child — identified by its snapshot index, which
 * never changes — or one this plan appended. `pathOfOriginal()` and
 * `pathOfAppended()` answer "what path addresses that child, given everything
 * emitted so far", which is the only question a plan ever has.
 *
 * Removals are the reason this exists at all: `merge-columns` removes its
 * later operands, and every index above a removal shifts DOWN. A plan that
 * computed its paths from the snapshot would address the wrong child from the
 * first removal on — and address one SUCCESSFULLY, which is silent
 * corruption rather than a failed action.
 */
class ChildList {
public:
    explicit ChildList( const LaneSnapshot &snap );

    int count() const { return slots_.size(); }

    /** Append a child, as `place-clip` and `split-clip` do. Returns its id. */
    int  append();
    /** Remove the child at `currentIndex`; every later index shifts down. */
    bool removeAt( int currentIndex );

    /** The current index of the original child that had snapshot index `i`. */
    int indexOfOriginal( int snapshotIndex ) const;
    /** The current index of the child `append()` returned `id` for. */
    int indexOfAppended( int id ) const;

    /** `lanePath + indexOfOriginal(i)`; empty when that child is gone. */
    QList<int> pathOfOriginal( int snapshotIndex ) const;
    /** `lanePath + indexOfAppended(id)`; empty when that child is gone. */
    QList<int> pathOfAppended( int id ) const;

private:
    // key = the child's REAL index on the lane at snapshot time, or an append id
    struct Slot { bool original; int key; };
    QList<int>  lanePath_;
    QList<Slot> slots_;
    int         nextAppendId_ = 0;

    int indexOf_( bool original, int key ) const;
};

/**
 * One record mode's plan.
 *
 * `plan()` appends the model actions for ONE lane to `out`, IN APPLY ORDER,
 * and returns false with `why` set when the lane cannot take the pass at all
 * (a system lane, a shape the mode refuses). Nothing is appended then, so a
 * refusal costs the caller no rollback — it is reported, and the recording's
 * file is listed as unused (plan 53 B6/Q13).
 */
class Planner {
public:
    virtual ~Planner() = default;
    virtual bool plan( const LaneSnapshot &snap, const Span &span,
                       const Context &ctx, SCompositeAction &out,
                       QString *why ) const = 0;
};

/**
 * The planner for a mode. A reference to a stateless singleton: a planner
 * holds nothing between passes, which is what makes it safe to hand the same
 * one to every armed lane of a pass.
 *
 * `Takes` is MODE A since P2 (QBX-180). `TakesSplit` and `Replace` are
 * QBX-172's. All three REFUSE a lane holding a wrapped take column (G11)
 * rather than splitting, folding or deleting a shape no gate has ever
 * exercised.
 */
const Planner &plannerFor( Mode mode );

/**
 * Read `lane` once, classifying every child. The ONE reader of lane state.
 *
 * SKIPPED OUTRIGHT: a child whose object `isLiveRecording()` — a growing clip
 * of this or another recorder. They are removed before the plan runs
 * (plan 53 §5.4), and skipping them here as well means a press that coincides
 * with another recorder's live clip cannot have that clip mistaken for an
 * edge or a column.
 *
 * IT READS THE MIX PARAMETERS TOO (`Child::mixVolume`), through
 * `SClipWindow::parametersOf` -- the same question `merge-columns` asks, so
 * that mode A can pre-check the merge's refusal rather than discover it from
 * inside a composite that then rolls the pass back.
 *
 * It does NOT call `stakes::normalizeColumns`: that takes the whole
 * `SProject&`, relinks and deletes wrappers OUTSIDE any action, and is a
 * post-load pass. Running it from inside an `apply()` would put a
 * non-undoable mutation in the middle of an undo step. After the post-load
 * pass any surviving wrapper is non-foldable anyway, so classifying it as
 * `WrappedColumn` loses nothing.
 */
LaneSnapshot snapshot( SObject *lane, const QList<int> &lanePath );

}  // namespace srecplan

#endif // _SRECORDPLANNER_H_
