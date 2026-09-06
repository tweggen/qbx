#ifndef SSENDLANEACTIONS_H
#define SSENDLANEACTIONS_H

#include "app/actions/saction.h"

/**
 * `add-send-lane` / `remove-send-lane` (proposal 45 M7 / D10).
 *
 * A send lane is a system lane with a plugin chain, a gain stage, a NAME and an
 * output that sums into the master. THESE VERBS BUILD THE LANE AND NOTHING
 * ELSE: there is no send TAP in this milestone, so nothing can feed one. Said
 * out loud rather than implied, because "add a send" reads like a routing
 * feature and this is a container.
 *
 * The NAME is the address (`$send:<name>`, AC7.2), which is why a collision is
 * REFUSED rather than uniquified: silently renaming a user's send to "Reverb 2"
 * would make every path they wrote address the wrong lane.
 *
 * `arrangement` is the root, as `add-track`'s is: these verbs take a NAME
 * rather than a path, so the root travels as its own attribute instead of as a
 * "Drums:..." qualifier.
 */
class SAddSendLaneAction : public SAction {
public:
    explicit SAddSendLaneAction( const QString &name = QString(),
                                 const QString &arrangement = QString() );

    QString name() const override { return QStringLiteral( "add-send-lane" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "sendName" ), QStringLiteral( "arrangement" ) }; }

private:
    QString sendName_;
    QString arrangement_;
};

/**
 * The inverse. Removes the LAST send lane, which is the only removal
 * `add-send-lane` can be the inverse of without renumbering somebody else's
 * address: sentinel `-2 - k` names the k-th lane, so removing one from the
 * middle would shift every lane after it and silently re-point every path that
 * named one. A general "remove this send" belongs with the routing milestone,
 * which is where a lane can have something pointing AT it to be re-pointed.
 */
class SRemoveSendLaneAction : public SAction {
public:
    explicit SRemoveSendLaneAction( const QString &name = QString(),
                                    const QString &arrangement = QString() );

    QString name() const override
    { return QStringLiteral( "remove-send-lane" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "sendName" ), QStringLiteral( "arrangement" ) }; }

private:
    QString sendName_;
    QString arrangement_;
};

#endif // SSENDLANEACTIONS_H
