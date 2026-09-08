#ifndef SMIXERTESTACTIONS_H
#define SMIXERTESTACTIONS_H

#include <QString>
#include <QStringList>

#include "app/actions/saction.h"

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
    QString control_ = QStringLiteral( "mute" );
    bool    on_      = true;
};

#endif // SMIXERTESTACTIONS_H
