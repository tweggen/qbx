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
//   <assert-locator position="96000"/>
//       A STATIC read of the current global locator, in frames. Unlike
//       wait-playhead this needs no playback: wait-playhead POLLS for
//       movement and requires the transport running, while Home/End (item j)
//       and Space/Shift+Space's resume-position (item o) move or read the
//       locator with the transport STOPPED.
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
//   <press-play shift="0"/>
//       Drives SMainWindow::startPlaying() (shift="0", the Space shortcut) or
//       startPlayingFromCurrent() (shift="1", Shift+Space) DIRECTLY — i.e. the
//       exact resume-position logic those two shortcuts run (item o), toggling
//       to a stop when already playing exactly as the real key does.
//       `toggle-playback` deliberately does NOT exercise this: it drives
//       SAppContext::setPlaybackRunning() straight, the older mechanism with no
//       resume-position distinction, so it cannot stand in for either shortcut
//       here. REJECTS when there is no main window (a headless run always has
//       one — see sactionrunner.cpp).
//
//   <press-stop/>
//       Drives SMainWindow::stopPlaying() DIRECTLY — the real Stop
//       button/shortcut. While playing it just stops; while ALREADY stopped it
//       resets the locator to 0, a mechanism that is DELIBERATELY separate
//       from item o's "last navigated" tracking (it does not call
//       noteUserNavigatedLocator). `toggle-playback play="0"` cannot stand in
//       for the second case: it stops the engine but has no
//       already-stopped branch at all. REJECTS when there is no main window.
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

class SAssertLocatorAction : public SAction {
public:
    SAssertLocatorAction() = default;
    explicit SAssertLocatorAction(qulonglong position) : position_(position) {}

    QString name() const override { return QStringLiteral("assert-locator"); }
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

class SPressPlayAction : public SAction {
public:
    SPressPlayAction() = default;
    explicit SPressPlayAction(bool shift) : shift_(shift) {}

    QString name() const override { return QStringLiteral("press-play"); }
    QStringList knownAttributes() const override { return {QStringLiteral("shift")}; }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    bool shift_ = false;
};

class SPressStopAction : public SAction {
public:
    QString name() const override { return QStringLiteral("press-stop"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;
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
