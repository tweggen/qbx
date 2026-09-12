#ifndef SMIXERTESTACTIONS_H
#define SMIXERTESTACTIONS_H

#include <QString>
#include <QStringList>

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"

/**
 * The MIXER PANE verbs (proposal 48 M1).
 *
 * The measurement lives in `SMainWindow` (`describeMixerPane` /
 * `describeMixerStrip` / `describeMixerLayout` / `mixerStripToggle`), not
 * here: `app/testkit` may not include `app/mixerui` any more than it may
 * include `app/timeline`. Same split as `assert-track-detail-layout` and
 * `assert-send-strip`.
 */

/// `assert-mixer-pane` — the strip LIST, or one strip's own state.
///
/// With no `track`, checks the pane: `strips`, `master`, `contains`, `absent`
/// against `strips=N|master=0|1|names=a,b,c`. With `track`, checks that one
/// strip's `describe()` instead. **`strips` may never be compared with the
/// arranger's visible ROW count** — the pane ignores fold, so the two
/// legitimately differ (proposal 48 D2, and `mixerui/CONTRACT.md` inv. 2).
class SAssertMixerPaneAction : public SAction
{
public:
    SAssertMixerPaneAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-mixer-pane" ); }
    QStringList knownAttributes() const override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString track_;       ///< empty = the pane; else the strip for this track
    QString arrangement_; ///< empty = the master (D12)
    int     strips_ = -1; ///< -1 = not checked
    int     master_ = -1; ///< -1 = not checked; 1 = a master strip is present
    QString contains_;
    QString absent_;
};

/// `assert-mixer-layout` — AC1.7's geometry gate, on BOTH axes.
///
/// Builds the pane off screen at `paneWidth` x `paneHeight` with strips at
/// `stripWidth`, settles it, and asserts `crushed` and `overlap`. A geometry
/// relation, never a screenshot: `screenshot` grabs the SCREEN's root window,
/// which is blank under `QT_QPA_PLATFORM=offscreen`.
class SAssertMixerLayoutAction : public SAction
{
public:
    SAssertMixerLayoutAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-mixer-layout" ); }
    QStringList knownAttributes() const override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int paneWidth_   = 640;
    int paneHeight_  = 260;
    int stripWidth_  = 96;
    int maxCrushed_  = 0;
    int maxOverlap_  = 0;
    int scrollNeeded_ = -1;   ///< -1 = not checked
    QString contains_;
};

/// `mixer-strip-toggle` — drive one strip's `mute` / `solo` / `arm` / `narrow`.
///
/// It CLICKS the real button and lets Qt deliver the signal, so a missing
/// `connect()` FAILS. A gesture verb: it adds no undo step of its own, because
/// the control's own handler submits the real action.
class SMixerStripToggleAction : public SAction
{
public:
    SMixerStripToggleAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "mixer-strip-toggle" ); }
    QStringList knownAttributes() const override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString track_;
    QString arrangement_;
    QString control_ = QStringLiteral( "mute" );
    bool    on_      = true;
};

/// `mixer-meter-tick` — ONE meter tick, driven the way `SApplication::meterTick`
/// drives one (proposal 48 M3).
///
/// **NO TRANSPORT.** `assert-meter` set the precedent: it requests the page
/// covering the position and runs the production `twLevelProbe` directly, so
/// the measurement is deterministic instead of racing a real playback run.
/// This verb does the same and then enters at exactly the point the broadcast
/// enters — `SMixerPane::onMeterTick` — so the dock gate, the audibility rule,
/// the live-owned branch, the lane sync and the probe binding are all the
/// production ones. A verb that pushed a level into the widget instead would
/// gate none of them.
class SMixerMeterTickAction : public SAction
{
public:
    SMixerMeterTickAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "mixer-meter-tick" ); }
    QStringList knownAttributes() const override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  arrangement_;
    offset_t position_    = 0;
    bool     live_        = true;
    bool     requestPages_ = true;
    qint64   nowMs_       = -1;   ///< -1 = a monotonically advancing default
};

/// `mixer-strip-set` — drive one strip's VALUE control (proposal 48 M3a).
///
/// The twin of `mixer-strip-toggle` for things that carry a number. It moves
/// the REAL fader and lets Qt deliver the signal, so `applyVolumeDb_`'s offer
/// to `SAutomationRecorder` is on the path — which is the whole of AC3a.1.
/// `gesture="double-click"` takes the reset route instead (AC3a.3).
class SMixerStripSetAction : public SAction
{
public:
    SMixerStripSetAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "mixer-strip-set" ); }
    QStringList knownAttributes() const override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString track_;
    QString arrangement_;
    QString control_ = QStringLiteral( "fader" );
    QString gesture_ = QStringLiteral( "set" );
    double  value_   = 0.0;
};

#endif // SMIXERTESTACTIONS_H
