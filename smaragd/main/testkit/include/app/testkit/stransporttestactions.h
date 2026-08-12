#ifndef STRANSPORTTESTACTIONS_H
#define STRANSPORTTESTACTIONS_H

#include "app/actions/saction.h"

#include <QList>

// Testkit transport verbs for headless playback tests:
//
//   <set-locator position="192000"/>
//       Moves the global locator (playback start position), in frames.
//
//   set-track-mute has MOVED to app/objects/track/ssettrackmuteaction.h — it is
//   a real, undoable edit now (the mute button submits it), no longer a
//   test-only verb. Its XML, index form included, is unchanged.
//
//   <wait-playhead minAdvance="240000" timeoutMs="15000"/>
//       Records the playhead, then pumps the event loop until it has
//       advanced by at least minAdvance frames. Rejects on timeout, so a
//       stalled playback fails the test.
//
//   <wait-playhead position="490000" timeoutMs="5000"/>
//       ABSOLUTE form: waits until the locator REACHES position. A case that
//       wants playback to arrive somewhere specific (so a decode at that
//       position means something) cannot express it as a relative advance
//       without knowing where playback started. Both attributes may be given,
//       in which case both conditions must hold. `position` was already being
//       written in a committed case and read by NOBODY — see
//       split_plain_screenshot.
//
//   <undo count="1"/>
//   <redo count="1"/>
//       Drives the real undo stack (SActionHistory), so a case can assert what
//       an action's INVERSE actually restores. Nothing covered that before, and
//       an inverse that silently rebuilt the wrong thing — or refused to apply
//       at all — looked identical to a working undo from outside.
//
// All are transient/test-support actions: not undoable themselves.

class SSetLocatorAction : public SAction {
public:
    SSetLocatorAction() = default;
    explicit SSetLocatorAction(qulonglong position) : position_(position) {}

    QString name() const override { return QStringLiteral("set-locator"); }
    QStringList knownAttributes() const override { return {QStringLiteral("position")}; }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    qulonglong position_ = 0;
};

class SWaitPlayheadAction : public SAction {
public:
    SWaitPlayheadAction() = default;
    SWaitPlayheadAction(qulonglong minAdvance, int timeoutMs)
        : minAdvance_(minAdvance), timeoutMs_(timeoutMs) {}

    QString name() const override { return QStringLiteral("wait-playhead"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("minAdvance"), QStringLiteral("position"),
                QStringLiteral("timeoutMs")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    qulonglong minAdvance_ = 0;
    qulonglong position_ = 0;   // 0 = no absolute target
    int timeoutMs_ = 10000;
};

class SUndoAction : public SAction {
public:
    SUndoAction() = default;
    explicit SUndoAction(int count) : count_(count) {}

    QString name() const override { return QStringLiteral("undo"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int count_ = 1;
};

class SRedoAction : public SAction {
public:
    SRedoAction() = default;
    explicit SRedoAction(int count) : count_(count) {}

    QString name() const override { return QStringLiteral("redo"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    int count_ = 1;
};

#endif // STRANSPORTTESTACTIONS_H
