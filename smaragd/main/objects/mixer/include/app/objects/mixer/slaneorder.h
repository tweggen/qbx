#ifndef SLANEORDER_H
#define SLANEORDER_H

#include <QVector>

#include "app/model/sobject.h"

class SLink;
class STrack;

/**
 * THE ONE FLATTENED LANE WALK (proposal 48 D1, M0).
 *
 * A depth-first list of a mixer's TRACK lanes, in the order the arranger draws
 * them: every user lane by `childLinks()` recursion, then the system-lane tail
 * (proposal 45 D11).  It exists so that the arranger's row builder and the
 * mixer pane -- and anything else that has to enumerate lanes -- share ONE
 * spelling of "which lanes are there, and in what order".
 *
 * WHY IT IS HERE AND NOT IN `app/model`.  It has to name `STrack`, and
 * `strack.h` lives two layers above `app/model`, whose include dirs a lower
 * layer cannot see.  `objects/mixer` is the lowest layer that can express the
 * type (it already declares `objects/track`), which is what makes this the
 * home rather than a preference.
 *
 * WHY IT IS ONE WALK.  The tree has folders, hidden lanes and a system-lane
 * tail that is NOT among `childLinks()`, and a second implementation is a
 * second place to forget the tail.  Proposal 45 D11 lists four specific ways
 * that goes wrong, and AC4.6 is one of them having actually happened: a walk
 * that missed the system tail pruned the master lane's own UI state on every
 * row rebuild.  This repository has paid for a duplicated rule four other
 * times (`main/timeline/CONTRACT.md` inv. 10, 32, 35), which is the whole
 * argument for the extraction.
 *
 * WHAT IT DOES NOT DO.  It produces LANES, never ROWS: no take sub-lanes, no
 * automation sub-lanes, no pixel heights.  Those are the arranger's business
 * and stay in `SStdMixerView::rebuildRows`, which folds them in per lane.  A
 * mixer pane consumes the list raw.
 */
namespace slaneorder {

/// Whether a COLLAPSED folder hides its subtree.  The arranger honours it (a
/// collapsed folder's children have no rows); a mixer pane does NOT (proposal
/// 48 D2 -- a mixer has no rows, so "show this lane's children as rows" is not
/// a question it can answer).
enum class Fold { Ignore, Honour };

/// Whether `SObject::laneHidden()` removes a lane and its subtree.  Both
/// mounts honour it for USER lanes: hiding answers "show this track at all",
/// which is not a per-mount question.  See `alwaysShowMaster` for the one
/// deliberate disagreement.
enum class Hidden { Ignore, Honour };

/// Which of the arrangement's system lanes the tail carries.
///
/// `MasterSubtree` is what the ARRANGER emits today: the master lane and,
/// unless it is collapsed, its own child lanes (which is where a conductor
/// lane lives -- proposal 45 M6).
///
/// **SEND LANES HAVE NO ARRANGER ROW, AND `All` IS THEREFORE UNUSED TODAY.**
/// Proposal 45's design text says the tail is "sends above, master last", but
/// `appendSystemRows()` only ever appended the master and nothing in
/// `sstdmixerview.cpp` mentions a send lane -- so a send lane created by
/// `add-send-lane` is in the model, carries a chain and a fader, and cannot be
/// seen or selected in the arranger.  That gap belongs to proposal 45 M7 and
/// closing it would change the arranger's row COUNT, which this milestone's
/// AC0.2 forbids.  `All` exists so the mixer pane can ask for the tail 45
/// specified without a second walk being written for it.
enum class SystemLanes { None, MasterSubtree, All };

struct Options {
    Fold        fold   = Fold::Honour;
    Hidden      hidden = Hidden::Honour;
    SystemLanes system = SystemLanes::MasterSubtree;

    /// PROPOSAL 48 D6a -- THE ONE PLACE THE TWO MOUNTS DELIBERATELY DISAGREE
    /// ABOUT A MODEL FLAG.  `laneHiddenByDefault()` is TRUE for every system
    /// role, so on a project nobody has touched the master lane has no
    /// arranger row -- correct for the arranger, where a master row costs
    /// vertical space in a list of lanes.  A mixer without its summing point
    /// is not a mixer, so the pane sets this and shows the master whatever the
    /// arranger's track list says (which is also what Pro Tools does).
    ///
    /// It exempts the master lane ITSELF only, never its children: a hidden
    /// conductor lane stays hidden in both mounts.
    bool alwaysShowMaster = false;
};

/// One track lane.  `link` is null for the master lane, which is not a child
/// of anything -- it is the mixer's own output stage (proposal 45 D2), and
/// `STrackRow` already carries a null link for it.
struct Lane {
    STrack     *track       = nullptr;
    SLink      *link        = nullptr;
    SObject    *parent      = nullptr;
    int         depth       = 0;
    bool        hasChildren = false;
    bool        collapsed   = false;
    SSystemRole role        = SSystemRole::None;
};

/// `root` is normally the `SStdMixer` being shown.  The system tail is emitted
/// only when `root` IS a mixer -- a nested container has none.
QVector<Lane> flattenTrackLanes( SObject *root, const Options &opt = Options() );

/// The index of `t` in the flattened list, or -1.  The ordering primitive
/// `strackbroadcast::orderByLane` is built on.
int indexOfTrack( const QVector<Lane> &lanes, const STrack *t );

}  // namespace slaneorder

#endif // SLANEORDER_H
