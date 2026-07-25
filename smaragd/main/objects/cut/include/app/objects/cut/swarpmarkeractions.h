#ifndef SWARPMARKERACTIONS_H
#define SWARPMARKERACTIONS_H

#include <vector>

#include <QList>

#include "app/actions/saction.h"
#include "tw/core/twwarpmap.h"

// Proposal 28 W2 — single-marker warp edits, the undoable form of the marker
// UI gestures (and the qxa verbs add-warp-marker / move-warp-marker /
// delete-warp-marker). Each applies through SCut::setWarpAnchors (sanitize,
// invalidate, rebuild — the W1 seam) and builds its exact inverse.
//
// Monotonicity discipline: an edit that would violate the strictly-
// increasing-in-both-domains invariant against its NEIGHBORS is REJECTED
// (apply fails) — never silently dropped or "fixed". The UI constrains drags
// to the legal range; scripts get an honest failure.

class SAddWarpMarkerAction : public SAction {
public:
    SAddWarpMarkerAction() = default;
    SAddWarpMarkerAction( const QList<int> &clipPath, int64_t src,
                          int64_t warped );

    QString name() const override { return QStringLiteral("add-warp-marker"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    int64_t    src_    = 0;
    int64_t    warped_ = 0;
};

class SMoveWarpMarkerAction : public SAction {
public:
    SMoveWarpMarkerAction() = default;
    // Moves the anchor identified by its SOURCE position to a new WARPED
    // position (the drag gesture: source identity fixed, timeline placement
    // moves). Changing the source side is delete+add.
    SMoveWarpMarkerAction( const QList<int> &clipPath, int64_t src,
                           int64_t newWarped );

    QString name() const override { return QStringLiteral("move-warp-marker"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    int64_t    src_       = 0;
    int64_t    newWarped_ = 0;
};

class SDeleteWarpMarkerAction : public SAction {
public:
    SDeleteWarpMarkerAction() = default;
    SDeleteWarpMarkerAction( const QList<int> &clipPath, int64_t src );

    QString name() const override { return QStringLiteral("delete-warp-marker"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    int64_t    src_ = 0;
};

#endif // SWARPMARKERACTIONS_H
