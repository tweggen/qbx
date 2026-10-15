#ifndef SCLEANUPTAKESACTION_H
#define SCLEANUPTAKESACTION_H

#include "app/actions/saction.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

class SCompositeAction;
class SProject;

/**
 * THE SWEEP'S PLAN, COMPUTED WITHOUT TOUCHING ANYTHING (plan 53 §6.3, QBX-195).
 *
 * `cleanup-takes` always decided the whole plan before removing anything --
 * deciding and removing in one pass would read indices a previous removal had
 * already renumbered. QBX-195 only took that decision OUT of `apply()`, so the
 * CONFIRMATION DIALOG can state what the sweep will do using the same numbers
 * the sweep will then do it by.
 *
 * That is the ticket's AC2 and it is not a preference: a dialog that counted
 * for itself would be a second reading of "which take came from a recording",
 * i.e. a second copy of decision D5 (a)'s name test, and the failure when they
 * drifted would be a confirmation that lied about the next click.
 *
 * It lives in `objects/cut` with the action because that is where `STakeStack`
 * and `stakenames` are. `app/servicesui` -- where the dialog lives -- may NOT
 * depend on this slice, which is why the dialog is handed the numbers rather
 * than the project: it is a presenter, and the caller (the arranger, which may
 * see both) is the one that asks.
 */
namespace scleanuptakes {

struct Plan {
    /// Per column: its path, and the take indices to remove, ASCENDING.
    QList<QPair<QList<int>, QList<int>>> perColumn;
    /// How many takes in total -- §6.3's "3 recordings".
    int         removedTakes   = 0;
    /// ...and how many were left alone because they are not recordings --
    /// §6.3's "1 original clip (kept)". Zero when `includeOriginals`.
    int         keptOriginals  = 0;
    /// How many columns contribute at least one removal, for the dialog's
    /// "across 2 clips".
    int         columnsTouched = 0;
    /// The names of the takes that would go, in plan order, for the dialog's
    /// detail list. A take with no name contributes "(unnamed)".
    QStringList removedNames;
};

/**
 * `clips` wins over `scope` exactly as the verb's own attributes do: a
 * non-empty ';'-separated path list IS the scope, and `scope == "project"`
 * walks the arrangement. Any other scope with no `clips` yields an EMPTY plan
 * and warns -- this slice cannot see a selection (see the class note below).
 */
Plan plan( SProject *project, const QString &pathRoot, const QString &scope,
           const QString &clips, bool includeOriginals );

}  // namespace scleanuptakes

/**
 * `cleanup-takes` — remove the takes a recording session left behind (plan 53
 * §6.3, Q13, decision D5 default (a)).
 *
 * Comping leaves rubbish. Every pass that was not used is still a take on its
 * column, and after an afternoon of punching a column can hold a dozen of them
 * with two in the comp. This sweeps the ones nothing selects.
 *
 * WHAT IT REMOVES, in this order, as ONE undoable macro:
 *
 *  1. UNUSED RECORDED TAKES. A take that is not the active take, that no comp
 *     segment selects over any position where it has material, and that CAME
 *     FROM A RECORDING.
 *  2. DANGLING COMP SEGMENTS: segments past the column's end. `remove-take`
 *     already renumbers the map and drops the removed take's segments (plan 53
 *     B10), so what is left for this step is the `normalized()` pass written
 *     back.
 *  3. A column left with one full-extent take COLLAPSES, which `remove-take`
 *     does on its own (invariant 3′).
 *
 * "CAME FROM A RECORDING" IS A NAME TEST, and that is decision D5 (a) rather
 * than an implementation shortcut: `stakenames::isRecorded` asks whether the
 * take's name carries the recorder's stamp (`"Take 3 · 14:27"`, plan 53 §8 P4).
 * A take that was the user's own pre-existing clip — W1's original A — is
 * EXEMPT, because it is their arrangement and not a recording artefact. The
 * dialog's "include original clips" box is `includeOriginals`.
 *
 * IT IS A HEURISTIC AND IS MEANT TO BE ONE (the model contract says so where
 * `stakenames` lives): a user who types `"Take 2 · 14:27"` by hand gets it
 * read as a recording. The alternative is a serialized per-take origin flag,
 * which is a file-format change D5 did not ask for, and the cost of a false
 * positive is one clip offered for removal that the confirmation lists by
 * name.
 *
 * SCOPE IS A PATH LIST AND NOT A SELECTION, which deviates from §6.3's
 * `scope="selection" | "project"` for a layering reason rather than a design
 * one: `objects/cut` may depend on `actions`, `model`, `objects/recplan`,
 * `objects/wave` and `persistence` — not on `shell` or `selection`, so an
 * action here cannot see what is selected. `scope="project"` IS resolvable
 * here (walk the arrangement), and the selection form becomes `clips=` with
 * the caller translating, which is the same split `split-selection` already
 * makes against `split-clip`.
 *
 * THE FILE HALF (`files="1"`) IS NOT UNDOABLE and says so. It moves orphan
 * recordings to the OS trash (`QFile::moveToTrash`, never a delete in place)
 * and is the one part of this verb that no case exercises — moving files about
 * inside a `-j4` suite is not something a test may do. The COUNT it would act
 * on is gated, through the same `srecfiles::scanUnused` the Unused recordings
 * dock reads; the deletion is hand-verified.
 */
class SCleanupTakesAction : public SAction {
public:
    SCleanupTakesAction() = default;
    SCleanupTakesAction( const QString &scope, const QString &clips,
                         bool includeOriginals, bool files );
    ~SCleanupTakesAction() override;

    QString name() const override
    { return QStringLiteral( "cleanup-takes" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /// How many takes the last apply() removed, for the caller's confirmation
    /// line. Read after submitting.
    int removedTakes() const { return removedTakes_; }
    /// ...and how many it left alone BECAUSE they were not recordings, which is
    /// the other half of §6.3's "3 recordings, 1 original clip (kept)".
    int keptOriginals() const { return keptOriginals_; }

private:
    QString scope_ = QStringLiteral( "project" );
    QString clips_;                      // ';'-separated clip paths
    bool    includeOriginals_ = false;
    bool    files_ = false;

    int removedTakes_   = 0;
    int keptOriginals_  = 0;

    /**
     * Orphan recordings to the OS trash. NOT on the undo stack, and the only
     * part of this verb no case exercises.
     */
    void binOrphanRecordings_( SProject *project );

    /**
     * THE APPLIED REMOVALS, KEPT ALIVE ON THIS ACTION.
     *
     * Each child is an `SRemoveTakeAction`, and since QBX-184 a removal of a
     * file-less take HOLDS A PIN on the window it took away -- the inverse
     * reads that pin back through a raw pointer to its forward action. So
     * deleting the macro after applying it would drop every pin and leave each
     * `SRestoreTakeAction` with a dangling owner: the undo would find nothing
     * pinned and refuse, i.e. the sweep would silently stop being undoable for
     * exactly the takes QBX-184 just made restorable.
     *
     * `SActionUndoCommand` keeps the FORWARD action for redo (its own header
     * says so), so a member here lives exactly as long as the undo step does.
     * This is `SRemoveTrackAction`'s "the pin lives on the persistent forward
     * action" one level up.
     */
    SCompositeAction *macro_ = nullptr;
};

#endif // SCLEANUPTAKESACTION_H
