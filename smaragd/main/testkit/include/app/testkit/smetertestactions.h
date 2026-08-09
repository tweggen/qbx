#ifndef SMETERTESTACTIONS_H
#define SMETERTESTACTIONS_H

#include "app/actions/saction.h"

#include <QString>

// Headless coverage for proposal 34's level meters.
//
// The whole point of the design is that levels are read from FROZEN PAGES BY
// POSITION rather than sampled as audio plays — which is exactly what makes it
// testable here. This verb needs no transport at all: it freezes the page for
// the position it is asked about (legal off the RT thread, deduped against any
// concurrent revalidation, the same requestPage() call the offline render makes)
// and then runs the production twLevelProbe over it. So it is deterministic,
// works under SMARAGD_REVAL_WORKERS=0, and does not depend on `toggle-playback`,
// which segfaults in scripts (testkit/CONTRACT.md known debt).
//
// assert-meter — assert the level the meter would show for a track at a position.
//   trackIndex = "0"        which track's root component to probe
//   position   = "0"        project frames; the probe measures the window ending
//                           here (twLevelProbe::MIN_WINDOW frames after a reset)
//   minRms / maxRms         bounds on the measured RMS (sqrt of the mean square)
//   minPeak / maxPeak       bounds on the measured peak
//   expectMiss = "false"    if true, the probe must find NO page (the miss path)
//   headHeight = "0"        if > 0, also build the real track head at this height
//                           and match `contains` against SLevelMeter::describe()
//                           via SMainWindow — the only automated coverage the
//                           density rules can have (testkit may not include
//                           app/timeline, so it hops through the shell).
//   contains   = ""         substring required in the head's meter description
//   grabPng    = ""         if set, ALSO paint a meter carrying the measured
//                           level into this PNG (under the test output dir) —
//                           the only coverage of SLevelMeter::paintEvent, and a
//                           human-checkable artifact of what the bar looks like
//   grabVertical = "true"   orientation of that grab
class SAssertMeterAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-meter" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    // Resolved lane address: trackPath_ when given, else the legacy top-level
    // trackIndex_. A one-element path IS a top-level index, so the two spell
    // the same thing for a flat project.
    QString effectivePath() const;

    QString trackPath_;          // empty = use trackIndex_
    int     trackIndex_ = 0;
    long long position_ = 0;
    double  minRms_  = -1.0;   // < 0 == not checked
    double  maxRms_  = -1.0;
    double  minPeak_ = -1.0;
    double  maxPeak_ = -1.0;
    bool    expectMiss_ = false;
    int     headHeight_ = 0;
    QString contains_;
    QString grabPng_;
    bool    grabVertical_ = true;
};

#endif  // SMETERTESTACTIONS_H
