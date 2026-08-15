#ifndef _SSETTRACKMIDIROUTINGACTION_H_
#define _SSETTRACKMIDIROUTINGACTION_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"

/**
 * `set-track-midi-routing` - how this track's events reach its parent
 * (proposal 36 3.2.1). ABSOLUTE, like every other track flag.
 *
 *   auto   - "consumed here, or bubbled up": the track passes its events up
 *            iff it has neither an instrument slot nor a MIDI-out port. This
 *            is REAPER's behaviour and the default.
 *   parent - force bubbling even past a local consumer (Cubase/Logic's
 *            explicit route, restricted to the hierarchy).
 *   none   - keep them local.
 */
class SSetTrackMidiRoutingAction : public SAction
{
public:
    SSetTrackMidiRoutingAction() = default;
    SSetTrackMidiRoutingAction( const QList<int> &trackPath,
                                const QString &routing );

    QString name() const override
    { return QStringLiteral( "set-track-midi-routing" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "routing" ) }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    routing_ = QStringLiteral( "auto" );
};

#endif // _SSETTRACKMIDIROUTINGACTION_H_
