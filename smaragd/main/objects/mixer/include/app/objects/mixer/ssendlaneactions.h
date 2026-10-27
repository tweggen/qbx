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
 * rather than a path, so the root is SPELLED as its own attribute instead of
 * as a "Drums:..." qualifier. It is STORED in SAction::pathRoot_ all the
 * same -- see the note on the members below, and QBX-249 for what a root kept
 * anywhere else costs.
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
    // THE ROOT IS SAction::pathRoot_, spelled as an `arrangement` attribute
    // because these verbs take a NAME rather than a path (QBX-249). It used to
    // be a member of this class, which is how add-track came to delete a
    // MASTER lane on undo: a root the funnels cannot see is dropped by
    // applyPropagatingRoot() and ignored by setPathRoot(). These three verbs
    // were CORRECT with a private member -- they pass it to every inverse by
    // hand -- and are migrated anyway, so that "the root lives in pathRoot_"
    // is a rule with no exceptions for a checker to have to carry.
    QString sendName_;
};

class STrack;

/**
 * The inverse. Removes the LAST send lane, which is the only removal
 * `add-send-lane` can be the inverse of without renumbering somebody else's
 * address: sentinel `-2 - k` names the k-th lane, so removing one from the
 * middle would shift every lane after it and silently re-point every path that
 * named one. A general "remove this send" belongs with the routing milestone,
 * which is where a lane can have something pointing AT it to be re-pointed.
 *
 * UNDO PRESERVES OBJECT IDENTITY (the SRemoveTrackAction idiom). The first
 * version of this verb returned `add-send-lane` as its inverse, which
 * re-created the lane BY NAME -- so a send lane removed and undone came back
 * EMPTY, losing its inserts, their state and its fader. That is data loss on a
 * plain Ctrl-Z, and "nothing can hear a send's chain yet" (M7/D10) is an
 * argument about the AUDIO, not about the user's work.
 *
 * So apply() pins the lane with an extra reference held ON THIS ACTION OBJECT,
 * which keeps it and its whole plugin chain alive and intact, and the inverse
 * (SRestoreSendLaneAction) re-adopts THAT object. The pin lives on the
 * persistent forward action -- the undo command reuses it -- and never on the
 * transient inverse the harness deletes after each undo/redo.
 */
class SRemoveSendLaneAction : public SAction {
public:
    explicit SRemoveSendLaneAction( const QString &name = QString(),
                                    const QString &arrangement = QString() );
    ~SRemoveSendLaneAction() override;

    QString name() const override
    { return QStringLiteral( "remove-send-lane" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "sendName" ), QStringLiteral( "arrangement" ) }; }

    /// Used by SRestoreSendLaneAction to read back and release the pinned lane.
    STrack *heldLane() const { return heldLane_; }
    void releaseHeld();

private:
    void dropStalePin();

    QString sendName_;          // the root is SAction::pathRoot_ (QBX-249)
    STrack *heldLane_ = nullptr;   // the removed lane, kept alive by holdsRef_
    bool    holdsRef_ = false;
};

/**
 * `remove-send-lane`'s inverse: re-adopt the pinned lane, with its chain.
 *
 * Never serialized standalone and not reconstructible from XML -- there is no
 * pinned object in a file -- exactly like SRestoreTrackAction, which it
 * mirrors. It re-adopts at the END of the send list, which IS the original
 * index because the removal only ever accepts the last lane; if that
 * restriction is ever lifted, this has to learn the index too.
 */
class SRestoreSendLaneAction : public SAction {
public:
    SRestoreSendLaneAction( SRemoveSendLaneAction *owner,
                            const QString &name = QString(),
                            const QString &arrangement = QString() );

    QString name() const override
    { return QStringLiteral( "restore-send-lane" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "sendName" ), QStringLiteral( "arrangement" ) }; }

private:
    SRemoveSendLaneAction *owner_ = nullptr;   // holds the pin (not owned)
    QString sendName_;          // the root is SAction::pathRoot_ (QBX-249)
};

#endif // SSENDLANEACTIONS_H
