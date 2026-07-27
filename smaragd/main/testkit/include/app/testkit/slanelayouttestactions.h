#ifndef SLANELAYOUTTESTACTIONS_H
#define SLANELAYOUTTESTACTIONS_H

#include "app/actions/saction.h"

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
 * <assert-lane-alignment/>
 */
class SAssertLaneAlignmentAction : public SAction {
public:
    SAssertLaneAlignmentAction() = default;

    QString name() const override { return QStringLiteral("assert-lane-alignment"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
};

#endif
