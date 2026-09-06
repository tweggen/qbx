#ifndef SSENDTAPACTIONS_H
#define SSENDTAPACTIONS_H

#include "app/actions/saction.h"
#include "app/model/ssendtap.h"

/**
 * `add-send` / `remove-send` / `set-send` — proposal 47 M0.
 *
 * A TAP is "this lane also feeds the send lane called <dest>". It lives on the
 * SOURCE object (D2) and names its destination, and it carries a level in dB
 * and a pre/post-fader choice (D3).
 *
 * **M0 IS MODEL ONLY. A tap added by these verbs is INAUDIBLE**, because
 * nothing wires a send bus until M1 — exactly as proposal 45 M7's send lane is
 * inaudible because nothing can feed it. Said out loud rather than implied: a
 * verb called `add-send` reads like a routing feature, and in this milestone it
 * edits a list.
 *
 * THE REFUSALS ARE THE SUBSTANCE OF THIS MILESTONE (D6), and each is ANNOUNCED
 * rather than silent (proposal 45 D6's own rule):
 *
 *  - an unresolvable SOURCE, or an empty destination;
 *  - a destination that is not a lane with `systemRole() == Send`;
 *  - a lane sending to ITSELF;
 *  - the MASTER lane as a source — the master's output is the sum that already
 *    contains every send lane, so master → send is a cycle whatever the tap
 *    graph looks like;
 *  - a CYCLE, by a reachability walk from the proposed destination back to
 *    the source over the existing taps.
 *
 * THE CYCLE REFUSAL IS NOT ABOUT A HANG, and an earlier version of this
 * comment said it was. It claimed the page scheduler would leave two nodes
 * each waiting on the other and that a render would sit until the watchdog.
 * MEASURED (proposal 47 M3): a cyclic project renders in about a second,
 * completes, and is byte-identical across worker counts --
 * `FreezeContext::isComponentInStack` breaks the recursion at render time.
 * The refusal is right for a different reason: the audio a broken cycle
 * produces is whatever that break happens to yield, which is defined by
 * accident rather than by anything a user asked for.
 *
 * AND THIS REFUSAL IS NOT THE ONLY DEFENCE, because the LOADER does not come
 * through here: `<sends>` is read by `SObject::readSends()`, so a hand-edited
 * or foreign project carries whatever it likes. `SStdMixer::rewireSendBuses()`
 * breaks any cycle that reached the model anyway, deterministically and out
 * loud. This layer exists to give the USER a message; that one exists to keep
 * the graph acyclic.
 *
 * Send → send is ALLOWED subject to that walk, deliberately. Refusing it
 * outright would be simpler and would remove a legitimate shape (a delay
 * feeding a reverb) for a hazard the walk already answers.
 */
class SAddSendAction : public SAction {
public:
    SAddSendAction( const QString &track = QString(),
                    const QString &dest = QString(),
                    double levelDb = 0.0, bool preFader = false,
                    bool enabled = true,
                    const QString &arrangement = QString() );

    QString name() const override { return QStringLiteral( "add-send" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "track" ), QStringLiteral( "dest" ),
               QStringLiteral( "level" ), QStringLiteral( "pre" ),
               QStringLiteral( "enabled" ), QStringLiteral( "arrangement" ) }; }

private:
    QString track_;
    QString dest_;
    double  levelDb_ = 0.0;
    bool    preFader_ = false;
    bool    enabled_ = true;
    QString arrangement_;
};

class SRemoveSendAction : public SAction {
public:
    SRemoveSendAction( const QString &track = QString(),
                       const QString &dest = QString(),
                       const QString &arrangement = QString() );

    QString name() const override { return QStringLiteral( "remove-send" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "track" ), QStringLiteral( "dest" ),
               QStringLiteral( "arrangement" ) }; }

private:
    QString track_;
    QString dest_;
    QString arrangement_;
};

/**
 * Change a tap that already exists. Only the attributes PRESENT are applied,
 * so `set-send level=` leaves the pre/post choice alone — the `set-lane-view`
 * shape, and the reason this is one verb rather than three: the three fields
 * are one control surface and a user dragging a send knob means one undo step.
 */
class SSetSendAction : public SAction {
public:
    SSetSendAction() = default;
    SSetSendAction( const QString &track, const QString &dest,
                    const QString &arrangement = QString() );

    QString name() const override { return QStringLiteral( "set-send" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "track" ), QStringLiteral( "dest" ),
               QStringLiteral( "level" ), QStringLiteral( "pre" ),
               QStringLiteral( "enabled" ), QStringLiteral( "arrangement" ) }; }

    void setLevelDb( double v )  { levelDb_ = v;  hasLevel_ = true; }
    void setPreFader( bool v )   { preFader_ = v; hasPre_ = true; }
    void setEnabled( bool v )    { enabled_ = v;  hasEnabled_ = true; }

private:
    QString track_;
    QString dest_;
    QString arrangement_;
    double  levelDb_ = 0.0;
    bool    preFader_ = false;
    bool    enabled_ = true;
    bool    hasLevel_ = false;
    bool    hasPre_ = false;
    bool    hasEnabled_ = false;
};

#endif
