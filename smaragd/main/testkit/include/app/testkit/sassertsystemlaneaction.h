#ifndef _SASSERTSYSTEMLANEACTION_H_
#define _SASSERTSYSTEMLANEACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-system-lane` — everything about a SYSTEM LANE that a script can
 * check without hearing it (proposal 45 M1).
 *
 * A system lane is a track the PROJECT owns rather than the user: the master,
 * a send, or a conductor lane. It is deliberately NOT among its root's
 * childLinks(), so `assert-track-count` cannot see it and no existing verb
 * can address it — which is exactly the property that has to be gated.
 *
 *   trackPath  = "$master"   the lane, in the sentinel spelling (D9). A
 *                            qualified form ("Drums:$master") names another
 *                            arrangement's lane.
 *   role       = "master"    none | master | send | conductor
 *   hidden     = ""          "1"/"0" — VIEW state, never an audio one
 *   acceptsClips = ""        "1"/"0" — the D6 policy predicate
 *   plugins    = "-1"        expected insert count on its chain (-1 = skip)
 *   volume     = ""          expected fader value in dB (exact; "" = skip)
 *   name       = ""          expected sName ("" = skip)
 *   expectPath = ""          THE ONE THAT BITES D9's WRITE SIDE. The path
 *                            `strackpath::pathOf()` derives for the lane,
 *                            written back through `pathToString()`. Before the
 *                            sentinel existed this was "" — the address of THE
 *                            ROOT MIXER — because pathOf() walks childLinks()
 *                            and the lane is not in them. Every track head
 *                            derives its commit address that way, so a master
 *                            fader would have moved and committed to the mixer
 *                            or to nothing. An assertion on the lane's
 *                            PROPERTIES cannot see that; only the round trip
 *                            can.
 *   armed      = ""          "1"/"0" — SObject::isArmedForRecording()
 *   monitor    = ""          off | auto | on — STrack::getMonitorMode()
 *   input      = ""          the raw portable trackInput string; the literal
 *                            "none" and "" both mean "no input", so a case
 *                            asserting the ABSENCE of one writes input="none"
 *   solo       = ""          "1"/"0" — SObject::isSolo()
 *   muted      = ""          "1"/"0" — SObject::isMuted(). Present because
 *                            AC5.4's claim is the OPPOSITE of the others': a
 *                            mute on the master is ACCEPTED and heard, so the
 *                            same verb has to be able to say "this one DID
 *                            take" as well as "these seven did not".
 *   midiOutPort = ""         STrack::getMidiOutPort(); "" is skip, so a case
 *                            asserting the port stayed empty writes
 *                            midiOutPort="<none>"
 *
 * THE SIX ABOVE ARE THE AC5.5 OBSERVABLES (proposal 45 M5). Every AC5.2
 * refusal has to leave the model untouched, and until they existed a case
 * could only assert that the verb RETURNED false — which a verb that refused
 * halfway through, after writing its field, would also do.
 *
 *   inChildLinks = ""        "0" asserts the lane is NOT a child link of its
 *                            root (D2): if it ever became one, every index
 *                            path in every case and fixture would shift by one
 *                            and mixer inv. 1 would be false.
 */
class SAssertSystemLaneAction : public SAction
{
public:
    SAssertSystemLaneAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-system-lane" ); }
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ),  QStringLiteral( "role" ),
               QStringLiteral( "hidden" ),     QStringLiteral( "acceptsClips" ),
               QStringLiteral( "plugins" ),    QStringLiteral( "volume" ),
               QStringLiteral( "name" ),       QStringLiteral( "expectPath" ),
               QStringLiteral( "inChildLinks" ),
               QStringLiteral( "armed" ),      QStringLiteral( "monitor" ),
               QStringLiteral( "input" ),      QStringLiteral( "solo" ),
               QStringLiteral( "muted" ),      QStringLiteral( "midiOutPort" ) }; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_    = QStringLiteral( "$master" );
    QString role_         = QStringLiteral( "master" );
    QString hidden_;
    QString acceptsClips_;
    int     plugins_      = -1;
    QString volume_;
    QString name_;
    QString expectPath_;
    QString inChildLinks_;
    QString armed_;
    QString monitor_;
    QString input_;
    QString solo_;
    QString muted_;
    QString midiOutPort_;
};

#endif // _SASSERTSYSTEMLANEACTION_H_
