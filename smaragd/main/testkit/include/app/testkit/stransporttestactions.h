#ifndef STRANSPORTTESTACTIONS_H
#define STRANSPORTTESTACTIONS_H

#include "app/actions/saction.h"

#include <QList>

// Testkit transport verbs for headless playback tests:
//
//   <set-locator position="192000"/>
//       Moves the global locator (playback start position), in frames.
//
//   <set-track-mute trackIndex="0" muted="0"/>
//   <set-track-mute trackPath="0,1" muted="1"/>
//       Sets the mute flag of a track, named either by root-mixer index or by
//       full track path. Only the path form reaches a track nested in a folder
//       track (a reparented track is no longer a mixer child).
//
//   <wait-playhead minAdvance="240000" timeoutMs="15000"/>
//       Records the playhead, then pumps the event loop until it has
//       advanced by at least minAdvance frames. Rejects on timeout, so a
//       stalled playback fails the test.
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
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    qulonglong position_ = 0;
};

class SSetTrackMuteAction : public SAction {
public:
    SSetTrackMuteAction() = default;
    SSetTrackMuteAction(int trackIndex, bool muted)
        : trackIndex_(trackIndex), muted_(muted) {}

    QString name() const override { return QStringLiteral("set-track-mute"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    // Either a top-level mixer index (trackIndex) or a full track path
    // (trackPath, e.g. "0,1" for the second lane of the first track). The path
    // form is what reaches a track NESTED in a folder track — a mixer index
    // cannot name one, and nested mute is exactly what needs covering since a
    // folder track sums its lanes itself. Path wins when both are given.
    int trackIndex_ = 0;
    QList<int> trackPath_;
    bool muted_ = false;

};

class SWaitPlayheadAction : public SAction {
public:
    SWaitPlayheadAction() = default;
    SWaitPlayheadAction(qulonglong minAdvance, int timeoutMs)
        : minAdvance_(minAdvance), timeoutMs_(timeoutMs) {}

    QString name() const override { return QStringLiteral("wait-playhead"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    qulonglong minAdvance_ = 0;
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
