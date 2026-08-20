#ifndef _SAUTOMATIONACTIONS_H
#define _SAUTOMATIONACTIONS_H

#include "app/actions/saction.h"
#include "app/model/sautomationlane.h"

#include <QList>
#include <QString>
#include <vector>

class SObject;
class SProject;

/**
 * THE AUTOMATION VERBS (proposal 37 P5, design §3.4).
 *
 * Every one of them is ABSOLUTE — like `set-pitch`, unlike a nudge — and every
 * one is undoable by carrying the pre-mutation state in its inverse.
 *
 * WHY THEY LIVE IN `objects/track` AND NOT IN `actions`. A lane owner is a
 * track, a plugin SLOT, or a clip window; only the slot needs a concrete type
 * (`SPluginChain::getSlotAt`), and that type lives here. Everything else
 * resolves through the model-level services (`splacements::laneAt` /
 * `placementAt`, `SObject::windowTakeAt`), so this slice already sees
 * everything the verbs need and `main/actions` stays model-only.
 *
 * ADDRESSING. `owner` is an index path from the root mixer, and WHICH KIND of
 * thing it names is decided by the TARGET's space rather than guessed from the
 * path — a `self:` target resolves it as a lane, a `cut:` target as a
 * placement, a `param:` target as a lane plus `slotIndex`. That is what makes
 * `owner="0,1"` unambiguous where "the second child of track 0" and "a nested
 * lane" would otherwise collide.
 */
namespace sautomation {

// One resolved lane owner.
struct OwnerRef {
    SObject  *owner = nullptr;
    SParamRef ref;
    bool valid() const { return owner != nullptr && ref.isValid(); }
};

/// `slotIndex` < 0 = not a slot; `take` < 0 = the ACTIVE take of a stack.
// The registered arrangement name a container belongs to, or empty for the
// master -- i.e. the qualifier that matches paths minted against `root`
// (proposal 09 D21). A UI that mints paths against its own view root must pass
// this, or its edits resolve in the master.
QString rootNameOf( SProject *project, SObject *root );

OwnerRef resolveOwner( SProject *project, const QString &pathRoot,
                       const QList<int> &ownerPath,
                       const QString &target, int slotIndex, int take );

/// The invalidation range an edit at `frame` produces on `lane`, already
/// widened to [start, INT64_MAX) when the consumer is CLASS 1 (a plugin
/// parameter — design F9). Call it BEFORE and AFTER the mutation and unite.
SAutomationLane::Range editRange( const SAutomationLane &lane,
                                  const SParamRef &ref, offset_t frame );

/// Commit: hand the owner the affected range so it pushes the new snapshot
/// into its engine components and stales exactly those pages.
void commit( OwnerRef &o, SAutomationLane &lane,
             const SAutomationLane::Range &r );

/// `<p t= v= c= k=/>` children, the one spelling every batch verb uses.
void writePointChildren( QDomElement &elem,
                         const std::vector<SAutomationPoint> &pts );
std::vector<SAutomationPoint> readPointChildren( const QDomElement &elem );

/// The lane a `set-track-volume` / `set-track-mute` must WRITE TO instead of
/// setting the static value: one that exists and whose mode consumes the curve
/// as the whole value (Read / Touch / Latch / Write). Null otherwise — Trim and
/// Off both leave the static value meaningful (design §11 decision 3).
SAutomationLane *readLaneFor( SObject *track, const QString &target );

/// Build the `set-automation-points` an overridden static edit becomes: one
/// point at `frame`, replacing whatever sat exactly there.
SAction *pointAtLocatorAction( const QList<int> &ownerPath, const QString &target,
                               offset_t frame, double value );

}  // namespace sautomation

// --- add / remove a lane -----------------------------------------------------
//
// ONE class, two verbs. `add-automation-lane` on a lane that already exists is
// an ABSOLUTE write of its mode and points (which is what makes the inverse of
// `remove-automation-lane` expressible at all: it carries the whole point list,
// design §3.4).
class SAddAutomationLaneAction : public SAction {
public:
    SAddAutomationLaneAction() = default;
    SAddAutomationLaneAction( const QList<int> &ownerPath, const QString &target,
                              SAutomationMode mode, int slotIndex, int take,
                              std::vector<SAutomationPoint> pts,
                              const QString &paramName );

    QString name() const override { return QStringLiteral( "add-automation-lane" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QList<int>                    ownerPath_;
    QString                       target_;
    QString                       paramName_;
    SAutomationMode               mode_ = SAutomationMode::Trim;
    int                           slotIndex_ = -1;
    int                           take_ = -1;
    std::vector<SAutomationPoint> points_;
    bool                          hasPoints_ = false;   // written by the inverse
};

class SRemoveAutomationLaneAction : public SAction {
public:
    SRemoveAutomationLaneAction() = default;
    SRemoveAutomationLaneAction( const QList<int> &ownerPath, const QString &target,
                                 int slotIndex, int take );

    QString name() const override { return QStringLiteral( "remove-automation-lane" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QList<int> ownerPath_;
    QString    target_;
    int        slotIndex_ = -1;
    int        take_ = -1;
};

// --- the mode ----------------------------------------------------------------
class SSetAutomationModeAction : public SAction {
public:
    SSetAutomationModeAction() = default;
    SSetAutomationModeAction( const QList<int> &ownerPath, const QString &target,
                              SAutomationMode mode, int slotIndex, int take );

    QString name() const override { return QStringLiteral( "set-automation-mode" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QList<int>      ownerPath_;
    QString         target_;
    SAutomationMode mode_ = SAutomationMode::Trim;
    int             slotIndex_ = -1;
    int             take_ = -1;
};

// --- single points -----------------------------------------------------------
//
// Points are addressed by their OLD (time, value) pair, design §3.4: a lane has
// no point ids, and a bare time would be ambiguous the moment a script wrote two
// points on one frame.
class SAddAutomationPointAction : public SAction {
public:
    SAddAutomationPointAction() = default;
    SAddAutomationPointAction( const QList<int> &ownerPath, const QString &target,
                               offset_t time, double value, twCurveShape shape,
                               double tension, int slotIndex, int take );

    QString name() const override { return QStringLiteral( "add-automation-point" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QList<int>   ownerPath_;
    QString      target_;
    offset_t     time_ = 0;
    double       value_ = 0.0;
    twCurveShape shape_ = twCurveShape::Linear;
    double       tension_ = 0.0;
    int          slotIndex_ = -1;
    int          take_ = -1;
};

class SRemoveAutomationPointAction : public SAction {
public:
    SRemoveAutomationPointAction() = default;
    SRemoveAutomationPointAction( const QList<int> &ownerPath, const QString &target,
                                  offset_t time, double value, int slotIndex, int take );

    QString name() const override { return QStringLiteral( "remove-automation-point" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QList<int> ownerPath_;
    QString    target_;
    offset_t   time_ = 0;
    double     value_ = 0.0;
    bool       matchValue_ = false;   // false = address by time alone
    int        slotIndex_ = -1;
    int        take_ = -1;
};

class SMoveAutomationPointAction : public SAction {
public:
    SMoveAutomationPointAction() = default;
    SMoveAutomationPointAction( const QList<int> &ownerPath, const QString &target,
                                offset_t time, double value,
                                offset_t toTime, double toValue,
                                int slotIndex, int take );

    QString name() const override { return QStringLiteral( "move-automation-point" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;
    QString mergeKey() const override;
    bool mergeWith( const SAction *later ) override;

private:
    QList<int> ownerPath_;
    QString    target_;
    offset_t   time_ = 0;
    double     value_ = 0.0;
    bool       matchValue_ = false;
    offset_t   toTime_ = 0;
    double     toValue_ = 0.0;
    int        slotIndex_ = -1;
    int        take_ = -1;
};

// --- the batch verb ----------------------------------------------------------
//
// THE coalescing one (design §3.4): curve drawing and every Touch/Latch/Write
// commit is one of these, never one action per block.
class SSetAutomationPointsAction : public SAction {
public:
    SSetAutomationPointsAction() = default;
    SSetAutomationPointsAction( const QList<int> &ownerPath, const QString &target,
                                offset_t from, offset_t to,
                                std::vector<SAutomationPoint> pts,
                                int slotIndex, int take );

    QString name() const override { return QStringLiteral( "set-automation-points" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;
    QString mergeKey() const override;
    bool mergeWith( const SAction *later ) override;

private:
    QList<int>                    ownerPath_;
    QString                       target_;
    offset_t                      from_ = 0;
    offset_t                      to_ = -1;      // < 0 = to the end of time
    std::vector<SAutomationPoint> points_;
    int                           slotIndex_ = -1;
    int                           take_ = -1;
};

#endif // _SAUTOMATIONACTIONS_H
