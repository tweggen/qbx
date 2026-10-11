#ifndef SSETTRACKRECORDMODEACTION_H
#define SSETTRACKRECORDMODEACTION_H

#include "app/actions/saction.h"
#include <QList>
#include <QString>

/**
 * Action: set one lane's record mode, or clear it back to INHERIT
 * (plan 53 §5.2, QBX-156 M0).
 *
 * `mode` is one of "takes", "takes-split", "overdub", "replace", or EMPTY
 * meaning inherit -- from the nearest ancestor folder track, else the project
 * (srecmode::resolveMode). Empty is the default, so a lane nobody has pinned
 * serializes no attribute and a project written before plan 53 re-serializes
 * byte-identically.
 *
 * ABSOLUTE, never a toggle: a script that says what it wants is idempotent and
 * can be read without counting how many times it ran (the `collapse-track`
 * rule, as set-lane-hidden follows it).
 *
 * The value is REFUSED rather than coerced when it names no mode. A typo that
 * silently meant "takes" would pin a lane to a mode the author did not choose
 * and look like it worked.
 *
 * Addressing is set-lane-hidden's: `trackPath` is an index-path from the root
 * mixer, and `$master` reaches a system lane. Setting a mode on a system lane
 * is pointless rather than harmful -- nothing records onto one -- and is left
 * permitted rather than guarded, because the resolver reads whatever is there
 * and no planner ever asks a system lane.
 *
 * Inverse: the same verb with the previous value, which restores INHERIT as
 * inherit (the empty string IS that state, so unlike set-lane-hidden there is
 * no separate explicitness flag to carry).
 */
class SSetTrackRecordModeAction : public SAction {
public:
    SSetTrackRecordModeAction() = default;
    SSetTrackRecordModeAction( const QList<int> &trackPath, const QString &mode )
        : trackPath_( trackPath ), mode_( mode ) {}

    QString name() const override
    { return QStringLiteral("set-track-record-mode"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    mode_;     // "" = inherit
};

#endif // SSETTRACKRECORDMODEACTION_H
