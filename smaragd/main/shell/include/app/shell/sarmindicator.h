#ifndef SARMINDICATOR_H
#define SARMINDICATOR_H

#include "app/shell/slanebadge.h"

class SObject;
class SProject;

/**
 * THE STATUS BAR'S RECORD-ARM INDICATOR, the solo badge's sibling.
 *
 * Asked for by the author beside QBX-257's solo badge: "the same display for
 * at least one track is armed would also come handy". It is the same display,
 * for the same reason and with the same failure mode in mind — an armed lane
 * can be off screen, in an arrangement tab nobody is looking at, on a lane
 * `set-lane-hidden` took away or inside a collapsed folder — and since QBX-260
 * the arm is also the thing that decides whether a monitor setting is in force
 * at all, so "what is armed" stopped being a question only the Record button
 * cares about: an arm is now why an input is open and why an instrument is
 * live.
 *
 * IT NAMES WHERE, exactly as the solo badge does: `ARM` when every armed lane
 * is in the root on screen, `ARM · Drums` when one is not, `ARM · Drums
 * (3 lanes)` for several in one other root, `ARM · 3 lanes` when they are
 * spread over several roots; the tooltip lists them by qualified path.
 *
 * AN INERT ARM STILL COUNTS. A project SAVED with a track armed comes back
 * armed but not live (QBX-260 design D9 keeps `SLiveMonitor` from opening the
 * microphone on a load), and this badge reports it anyway: that is what the
 * track's own Arm button shows, and a badge disagreeing with the button would
 * be the worse lie. `sarmcensus.h` carries the same note.
 *
 * NOT A SECOND SPELLING OF "IS RECORDING". Recording is a transport state and
 * the transport bar owns it; this is the SET of lanes a record pass would
 * capture into, which is exactly what is invisible when the lane is elsewhere.
 */
class SArmIndicator : public SLaneBadge {
    Q_OBJECT

public:
    explicit SArmIndicator( QWidget *parent = nullptr );

    /** Recompute from the model; the arguments are the solo badge's, and mean
     *  the same things (see SLaneBadge::refreshFrom). */
    void refresh( SProject *project, SObject *activeRoot,
                  const QString &masterLabel );

    /**
     * `armShown=<0|1>|armText=<text>|armLanes=<n>|armRoots=<a;b>
     *  |armOnScreen=<0|1>|armList=<spec ...>` — SLaneBadge's five fields with
     * this badge's prefix, then the list, fields APPENDED and never inserted.
     */
    QString describe() const;
};

#endif  // SARMINDICATOR_H
