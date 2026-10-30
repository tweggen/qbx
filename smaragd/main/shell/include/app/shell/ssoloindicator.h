#ifndef SSOLOINDICATOR_H
#define SSOLOINDICATOR_H

#include <QStringList>
#include <QToolButton>

class SObject;
class SProject;

/**
 * THE STATUS BAR'S SOLO INDICATOR (QBX-257).
 *
 * In the default `global` solo scope (QBX-256) a solo's EFFECT is
 * project-wide while its CAUSE can be off screen: in an arrangement tab the
 * user closed, on a lane hidden by `set-lane-hidden`, or inside a collapsed
 * folder. That is silent state with a global effect, and the first encounter
 * with it is "why is everything quiet?" — with the answer in a tab nobody is
 * looking at.
 *
 * IT IS A BUTTON, NOT A LAMP. An indicator that only informs leaves the user
 * to hunt for the lane, and the hunt IS the problem; clicking releases every
 * solo in the project (SMainWindow::clearAllSolos, one undo step).
 *
 * IT NAMES WHERE. `SOLO` when the soloed lane is in the root on screen,
 * `SOLO · Drums` when it is not, `SOLO · Drums (3 lanes)` for several in one
 * other root, `SOLO · 3 lanes` when they are spread over several roots. The
 * root name is the whole value of the thing — it is what saves the hunt — and
 * the tooltip lists the lanes by qualified path.
 *
 * IT ASKS NO QUESTION OF ITS OWN. Which lanes are soloed comes from
 * `ssolo::soloedLanesInProject()` and the scope from `SProject::soloScope()`;
 * where a lane lives comes from `splacements::specOf()` /
 * `splacements::rootHolding()`. ssolorules.h's own header records why that
 * matters: the audibility rule was written out three times and all three were
 * wrong in the same way.
 *
 * NOT COLOUR ALONE. The word `SOLO` carries the state; the colour is
 * decoration, and it is taken from the palette's Highlight role rather than
 * written as a hex literal, so it follows the light and the dark theme
 * instead of being legible in one of them.
 */
class SSoloIndicator : public QToolButton {
    Q_OBJECT

public:
    explicit SSoloIndicator( QWidget *parent = nullptr );

    /**
     * Recompute everything from the model. `activeRoot` is the root the user
     * is looking at (SViewTabs::activeRoot()) and decides only whether the
     * root is NAMED, never whether the badge appears: a solo the user cannot
     * see is the case this widget exists for. `masterLabel` is what the
     * master's own tab is called, so the badge names the master the way the
     * tab bar does rather than inventing a second word for it.
     */
    void refresh( SProject *project, SObject *activeRoot,
                  const QString &masterLabel );

    /** True while at least one lane in the project carries solo. */
    bool inForce() const { return lanes_ > 0; }

    /**
     * The test seam, one line, fields APPENDED and never inserted (the house
     * rule: assert-status-bar matches substrings, so a field put between two
     * existing ones breaks every assertion spanning the seam).
     *
     *   soloShown=<0|1>      !isHidden() — the WIDGET's own state, not our
     *                        intent, so a build that computes the label and
     *                        forgets to hide the badge cannot pass
     *   soloText=<text>      text(), i.e. exactly what is painted
     *   soloLanes=<n>        how many lanes carry solo, whole project
     *   soloRoots=<a;b>      the roots holding them, display labels, in root
     *                        order, deduped, ';'-joined (a name containing
     *                        ';' is written as is — this field is for gates)
     *   soloOnScreen=<0|1>   every soloed lane is in the root on screen
     *   soloScope=<global|arrangement>
     *   soloList=<spec ...>  the qualified paths the tooltip lists,
     *                        space-joined
     */
    QString describe() const;

private:
    void apply_( const QString &text, const QString &tip );

    int         lanes_ = 0;
    QStringList roots_;
    QStringList specs_;
    QString     scope_;
    bool        onScreen_ = true;
};

#endif  // SSOLOINDICATOR_H
