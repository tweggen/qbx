#ifndef SLANELAYOUTTESTACTIONS_H
#define SLANELAYOUTTESTACTIONS_H

#include "app/actions/saction.h"
#include <QString>

/**
 * Move the arranger's vertical geometry: vertical zoom (base lane height),
 * vertical scroll (top lane), a track's individual lane height, and whether a
 * track shows its take lanes. Every attribute is optional.
 *
 * These are VIEW state, not project state — the action is not undoable and
 * changes nothing that is saved. It exists so a test can put the view into the
 * states in which the track heads used to come unstuck from the lanes.
 *
 * XML format:
 * <set-lane-view trackHeight="60" topRow="2"
 *                laneScaleRow="0" laneScale="2.5"
 *                toggleTakesRow="1"/>
 *
 * Since proposal 37 P6 it also drives the AUTOMATION lane state, because that
 * is view state of exactly the same kind (per-track, not saved, pruned with the
 * track): which lanes a track shows, and whether clip-envelope editing is
 * armed. `showAutomation="1"` shows, `"0"` hides.
 *
 * <set-lane-view automationTrack="0" automationTarget="self:Volume"
 *                automationSlot="-1" showAutomation="1"/>
 * <set-lane-view clipEnvelopes="1"/>
 *
 * `secondWidth` (horizontal zoom, px/second) and `scrollX` (horizontal pan,
 * project frames) are the ONE exception to "changes nothing that is saved"
 * above (fix/track-list-polish m): they drive
 * `SMVActualView::setSecondWidth()`/`setLeftOffset()` directly, the same
 * calls the zoom buttons and the wheel make, which is exactly what
 * `saveViewStateToProject()` is hooked to — so setting them here is how a
 * case puts the view into a state worth a save/load round-trip case.
 *
 * <set-lane-view secondWidth="45.0" scrollX="12345"/>
 */
class SSetLaneViewAction : public SAction {
public:
    SSetLaneViewAction() = default;

    QString name() const override { return QStringLiteral("set-lane-view"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int    laneScaleRow_ = -1;
    double laneScale_ = 0.0;
    int    toggleTakesRow_ = -1;
    int    baseTrackHeight_ = 0;
    int    topRow_ = -1;
    QString automationTrack_;
    QString automationTarget_;
    int    automationSlot_ = -1;
    int    showAutomation_ = -1;      // -1 = untouched
    int    clipEnvelopes_ = -1;       // -1 = untouched
    double secondWidth_ = 0.0;        // <= 0 = untouched
    qlonglong scrollX_ = -1;          // < 0 = untouched
};

/**
 * Fail unless every track head sits exactly on its lane — same top, same
 * height, full column width — and the lane geometry is self-consistent (row
 * tops are the running sum of the row heights, and the y -> row lookup inverts
 * it at both edges of every lane).
 *
 * This is the invariant the arranger broke: heads are placed by
 * SStdMixerView::layoutControlColumn() while lanes are drawn by the canvas, and
 * the two used to derive their y from different formulas. Assert it AFTER
 * moving the view (see set-lane-view) — that is when they came apart.
 *
 * XML format:
 * Since proposal 37 P6 the same check covers AUTOMATION sub-lanes: they are
 * sub-lanes by the same rule take lanes are, so a track that owns several of
 * them still gets ONE head spanning the whole group. The check additionally
 * requires every automation row to name a lane the model can still resolve —
 * a row left behind by a removed lane would paint an empty band with no way to
 * tell it from a lane that is simply flat.
 *
 * `grabPng` paints the arranger CANVAS into the test output directory.
 * Coverage, not an oracle (design §6.4) — it is the only thing that proves
 * drawAutomationLane draws at all.
 *
 * XML format:
 * <assert-lane-alignment/>
 * <assert-lane-alignment grabPng="lanes.png" grabWidth="900" grabHeight="400"/>
 */
class SAssertLaneAlignmentAction : public SAction {
public:
    SAssertLaneAlignmentAction() = default;

    QString name() const override { return QStringLiteral("assert-lane-alignment"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString grabPng_;
    int     grabWidth_ = 0;
    int     grabHeight_ = 0;
};


/**
 * `collapse-track` - fold a folder lane SHUT, or open it again (proposal 39
 * M3a).
 *
 * VIEW state, exactly like set-lane-view: not undoable, saved nowhere. It
 * exists because the folder-sum overlay is SOLD on the collapsed folder - fold
 * it shut and you can still see what is under it - and nothing in the testkit
 * reached the fold at all, so M3's pixel gate necessarily grabbed an EXPANDED
 * folder.
 *
 * It goes out through SMainWindow (testkit may not include app/timeline,
 * testkit CONTRACT inv. 5) to SStdMixerView::toggleTrackCollapsed(), the same
 * call the head's fold triangle makes - not to a second writer of the collapsed
 * set, which would be free to skip the row rebuild that IS the fold.
 *
 * `collapsed` is ABSOLUTE, not a toggle: a script that says what it wants is
 * idempotent, and it can be read without counting how many times it ran.
 *
 * WHAT IS OBSERVABLE afterwards, because there is no row-count probe and this
 * verb does not invent one: the children's rows are GONE, so every lane BELOW
 * the folder moves up by that many rows - which `assert-lane-overlay`'s
 * `contains="row=N"` reads straight off its own report line. And the folder's
 * own lane is still painted by the same renderer, so `assert-lane-overlay` on
 * it still finds the overlay. That pair is the user story.
 *
 * XML format:
 * <collapse-track trackPath="0" collapsed="1"/>
 * <collapse-track trackPath="0" collapsed="0"/>
 */
class SCollapseTrackAction : public SAction {
public:
    SCollapseTrackAction() = default;

    QString name() const override { return QStringLiteral("collapse-track"); }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral("trackPath"), QStringLiteral("collapsed") };
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString trackPath_ = QStringLiteral("0");
    bool    collapsed_ = true;
};

/**
 * `assert-lane-view` (fix/track-list-polish m) - read back the two pieces of
 * state that item (m) makes persist across save/load: a track's fold state
 * (STrack::isCollapsed(), unrelated to any arranger being open) and the
 * arranger's zoom/pan (SMVActualView::secondWidth_/upperLeftOffset_, which
 * only exist once an arranger has been built).
 *
 * Every attribute is optional and independent, exactly like set-lane-view:
 * give `trackPath` + `collapsed` to check fold state, `secondWidth` and/or
 * `scrollX` to check zoom/pan, any combination in one call.
 *
 * `secondWidth` compares to a tolerance (it is a double, px/second);
 * `scrollX` compares EXACTLY (project frames, an integer quantity).
 *
 * XML format:
 * <assert-lane-view trackPath="0" collapsed="1"/>
 * <assert-lane-view secondWidth="45.0" scrollX="12345"/>
 */
class SAssertLaneViewAction : public SAction {
public:
    SAssertLaneViewAction() = default;

    QString name() const override { return QStringLiteral("assert-lane-view"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString trackPath_;
    int     expectCollapsed_ = -1;      // -1 = not checked
    double  expectSecondWidth_ = -1.0;  // < 0 = not checked
    qlonglong expectScrollX_ = -1;      // < 0 = not checked
    // RELATIVE pan checks (same shape as assert-envelope's, and for the same
    // reason): `snapshot` records the current scrollX under a name and asserts
    // nothing; `compareTo` asserts the current scrollX EQUALS the named one,
    // and with the script-level `expectReject="true"` that it DIFFERS.
    //
    // They exist because an ABSOLUTE scrollX is not a property of the code
    // under test. One wheel notch pans `span/8 * wheelSensitivity_`, and that
    // sensitivity is a USER SETTING (SOpt::WheelSensitivityPct, Edit ->
    // Options -> Mouse). A case that hard-codes the landing position therefore
    // passes only on a box left at the 100 % default -- which is how
    // follow_scroll_hold came to fail on the author's own machine, where the
    // arranger wheel sensitivity is 160 %: it expected 2580 and measured
    // 2580 * 1.6 = 4128 exactly. What that case is actually about is whether
    // the view MOVED, and these two attributes say that without naming a
    // number the user is free to change.
    QString snapshot_;
    QString compareTo_;
    qlonglong minScrollX_ = -1;         // < 0 = not checked
};

/**
 * `assert-scroll-range` (fix/track-list-polish l) - the NUMERIC gate on the
 * scroll-padding fix: after scrolling the vertical scrollbar to its OWN
 * maximum (the number a real mouse wheel is capped by, via
 * `SMainWindow::arrangerDescribeScrollRange` / `tkVerticalScrollMaximum`),
 * is the true LAST row now fully visible in the canvas?
 *
 * Deliberately not `set-lane-view topRow=<rowCount-1>`: that call clamps only
 * to `rowCount()-1` and bypasses the scrollbar's `maximum()` entirely, so it
 * cannot see the bug this fixes (a `maximum()` computed one row too low,
 * which without the fix leaves the last row permanently clipped no matter
 * how far a wheel scrolls).
 *
 * `expectFullyVisible` = "" (not checked; "true"/"false" otherwise),
 * `expectMaxAtLeast` = "-1" (< 0 = not checked; >= 0 asserts
 * `maxScroll >= N` — there IS scroll room, which is what distinguishes the
 * fix from simply having nothing to scroll for), `expectMaxAtMost` = "-1"
 * (< 0 = not checked; the negative control — a project whose content
 * already fits the viewport must not gain unwanted scroll room, e.g.
 * `expectMaxAtMost="0"`).
 *
 * XML format:
 * <assert-scroll-range expectFullyVisible="true" expectMaxAtLeast="1"/>
 * <assert-scroll-range expectMaxAtMost="0"/>
 */
class SAssertScrollRangeAction : public SAction {
public:
    SAssertScrollRangeAction() = default;

    QString name() const override { return QStringLiteral("assert-scroll-range"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString expectFullyVisible_;   // "" = not checked
    int     expectMaxAtLeast_ = -1;
    int     expectMaxAtMost_ = -1;
};

#endif
