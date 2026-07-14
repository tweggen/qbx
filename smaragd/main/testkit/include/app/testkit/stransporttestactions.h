#ifndef STRANSPORTTESTACTIONS_H
#define STRANSPORTTESTACTIONS_H

#include "app/actions/saction.h"

// Testkit transport verbs for headless playback tests:
//
//   <set-locator position="192000"/>
//       Moves the global locator (playback start position), in frames.
//
//   <set-track-mute trackIndex="0" muted="0"/>
//       Sets the mute flag of the track at the given root-mixer index.
//
//   <wait-playhead minAdvance="240000" timeoutMs="15000"/>
//       Records the playhead, then pumps the event loop until it has
//       advanced by at least minAdvance frames. Rejects on timeout, so a
//       stalled playback fails the test.
//
// All three are transient/test-support actions: not undoable.

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
    int trackIndex_ = 0;
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

#endif // STRANSPORTTESTACTIONS_H
