#ifndef SARRANGEMENTTESTACTIONS_H
#define SARRANGEMENTTESTACTIONS_H

#include "app/actions/saction.h"
#include <QString>

// Testkit verbs for the arrangement registry (proposal 09 M1).
//
// WHY THESE ARE VERBS AND NOT `<assertions>` KINDS, because it is the whole
// reason they exist: the runner recognizes exactly two assertion KINDS
// (SActionRunner::evaluateAssertions_) and evaluates them AFTER the entire
// action list has run. So an assertion kind cannot be interleaved with undo,
// with an edit, or with anything else — and the existing `assert-track-count`
// kind additionally reads attribute `equals` (not `count`) and resolves
// `project->getRootComponent()` through a `dynamic_cast<SStdMixer*>`, so it can
// only ever see the MASTER root's top-level lanes.
//
// Every gate in this proposal needs the opposite of all three: mid-script,
// per-arrangement, and interleaved with undo. The old kind keeps working for
// the cases that already use it; nothing is migrated here.

// <assert-arrangements names="Drums,Bass" trackCounts="2,1"/>
// `names` is compared as a SET (the registry is a hash; SProject::arrangementNames()
// sorts, and so does this). `trackCounts` is optional and positional against the
// SORTED name list. An empty `names` asserts there are no arrangements at all.
class SAssertArrangementsAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-arrangements" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("names"), QStringLiteral("trackCounts") }; }

private:
    QString names_;
    QString trackCounts_;
};

// <assert-track-count arrangement="Drums" count="2"/>
// The root-aware, in-sequence counterpart of the old assertion kind.
// `arrangement` empty (or absent) means the MASTER root, so this verb can carry
// a master-side assertion in the same script as an arrangement-side one.
// Counts the root's DIRECT lanes, matching SStdMixer::getNTracks().
class SAssertArrangementTrackCountAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-track-count" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("arrangement"), QStringLiteral("count") }; }

private:
    QString arrangement_;
    int     count_ = -1;
};

#endif  // SARRANGEMENTTESTACTIONS_H
