#ifndef SSTATUSBARTESTACTIONS_H
#define SSTATUSBARTESTACTIONS_H

#include "app/actions/saction.h"

// THE STATUS BAR'S PERMANENT WIDGETS (QBX-257).
//
//   <assert-status-bar contains="soloRoots=Drums" absent="soloShown=0"/>
//       Reads SMainWindow::describeStatusBar() and matches SUBSTRINGS of it,
//       the same shape as assert-transport-readout. The line is
//
//         mode=<…>|soloShown=<0|1>|soloText=<…>|soloLanes=<n>
//                 |soloRoots=<a;b>|soloOnScreen=<0|1>
//                 |soloScope=<global|arrangement>|soloList=<spec …>
//                 |clearSolosEnabled=<0|1>
//                 |armShown=<0|1>|armText=<…>|armLanes=<n>|armRoots=<a;b>
//                 |armOnScreen=<0|1>|armList=<spec …>
//                 |disarmAllEnabled=<0|1>
//
//       `soloRoots` is THE DISCRIMINATOR for the defect this gates: it names
//       the root that holds the solo, so an implementation that read the
//       MASTER root (the mistake QBX-231/233/243 keep finding) cannot answer
//       "Drums" for a solo the master cannot see.
//
//       The `arm*` fields are the RECORD-ARM badge's, the solo badge's
//       sibling, and mean the same things one flag over: `armRoots` is the
//       same discriminator (the root HOLDING the armed lane, not the master),
//       and `armLanes` counts an arm that came back INERT from a saved
//       project too -- which is what the track's own Arm button shows.
//
//       The FIRST read attaches BOTH indicators -- a scripted run never
//       reaches the attach sites -- so it has to come before the gestures it
//       must follow, exactly as assert-track-detail-dock does.
//
//   <click-solo-indicator/>
//       A REAL click on the badge, through the same QAction the Edit menu
//       item triggers, which clears every solo in the project as one undo
//       step. It REJECTS when that action is disabled, so
//
//         <click-solo-indicator expectReject="true"/>
//
//       is how a case states "with no solo engaged the badge does nothing".
//       The refusal is the SEAM's own: QAction::trigger() emits whatever the
//       enabled state, so a seam that merely triggered would pass over a
//       build that greys nothing.
//
//   <click-arm-indicator/>
//       The same thing for the record-arm badge: a real click, through the
//       same QAction the Edit menu's "Disarm all tracks" triggers, which
//       disarms every armed lane in the project as one undo step. It refuses
//       the same way when nothing is armed.
//
// None of them is undoable itself; the clear's and the disarm's undo is the
// macro's, on the real undo stack (so `undo` / `verify-undo` reach it).

class SAssertStatusBarAction : public SAction {
public:
    QString name() const override { return QStringLiteral("assert-status-bar"); }
    QStringList knownAttributes() const override;
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString contains_;
    QString absent_;
};

class SClickSoloIndicatorAction : public SAction {
public:
    QString name() const override { return QStringLiteral("click-solo-indicator"); }
    QStringList knownAttributes() const override;
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
};

class SClickArmIndicatorAction : public SAction {
public:
    QString name() const override { return QStringLiteral("click-arm-indicator"); }
    QStringList knownAttributes() const override;
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
};

#endif  // SSTATUSBARTESTACTIONS_H
