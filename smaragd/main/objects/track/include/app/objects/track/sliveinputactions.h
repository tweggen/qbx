#ifndef _SLIVEINPUTACTIONS_H_
#define _SLIVEINPUTACTIONS_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"
#include "app/objects/track/strack.h"

/**
 * The three live-input verbs (proposal 21 L1b, design D9). All ABSOLUTE and
 * all undoable: each carries the whole new state and its inverse carries the
 * whole previous state, exactly like `set-track-midi-output`.
 *
 * Each one ends with `SAppContext::get().liveLanesChanged()` — a
 * NOTIFICATION, never the work. The arm/disarm sequence (retire the closure's
 * nodes → `setLiveOwned(true)` → wire the exclusion + bump → read the root
 * epoch → publish the plan → `openLive`) has to happen in ONE place with the
 * pump and the speaker in reach, and that place is `SLiveMonitor` in the
 * shell. A verb that tried to do it here would be a second, competing
 * ordering, and the ordering is the whole of design D3.
 */

/// `arm-track` — record-arm, which is also monitor-arm (design D9: one flag).
class SArmTrackAction : public SAction
{
public:
    SArmTrackAction() = default;
    SArmTrackAction( const QList<int> &trackPath, bool armed );

    QString name() const override { return QStringLiteral( "arm-track" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "armed" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    bool       armed_ = false;
};

/// `set-track-input` — the per-track input selector, a PORTABLE string.
class SSetTrackInputAction : public SAction
{
public:
    SSetTrackInputAction() = default;
    SSetTrackInputAction( const QList<int> &trackPath, const QString &input );

    QString name() const override { return QStringLiteral( "set-track-input" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "input" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    input_;
};

/// `set-monitor-mode` — auto | on | off.
class SSetMonitorModeAction : public SAction
{
public:
    SSetMonitorModeAction() = default;
    SSetMonitorModeAction( const QList<int> &trackPath, STrack::MonitorMode mode );

    QString name() const override { return QStringLiteral( "set-monitor-mode" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "mode" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int>          trackPath_;
    STrack::MonitorMode mode_ = STrack::MonitorMode::Auto;
};

#endif // _SLIVEINPUTACTIONS_H_
