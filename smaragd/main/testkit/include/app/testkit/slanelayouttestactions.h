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

#endif
