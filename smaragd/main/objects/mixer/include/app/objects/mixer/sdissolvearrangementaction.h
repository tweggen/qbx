#ifndef SDISSOLVEARRANGEMENTACTION_H
#define SDISSOLVEARRANGEMENTACTION_H

#include "app/actions/saction.h"
#include "app/model/slink.h"
#include <QList>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QVariant>

class SObject;
class STrack;

// Action: bring an arrangement's tracks back into the master, removing its
// asset, its snippets and its placements, and unregistering the now-empty root.
// The exact inverse of extract-arrangement, and a first-class user verb in its
// own right ("put these back in the song") rather than a private undo helper.
//
//   <dissolve-arrangement name="Drums"/>
//
// restorePlan is how an UNDO of extract-arrangement puts the tracks back
// exactly where they came from: one "parentPath@index" per track, in the
// arrangement's own track order, semicolon-separated ("@" rather than ":" so
// it cannot be read as a root qualifier). It is written by extract-arrangement
// when it builds this as its inverse. Invoked by a user with no plan, the
// tracks are APPENDED to the master instead — which is the honest answer when
// nothing recorded where they used to be.
//
// REFUSED above one placement of the asset. extract-arrangement creates exactly
// one, so as an inverse this can never fire; but a user who placed the asset a
// second time and then dissolved would otherwise get one placement silently
// restored and the rest deleted. Removing the extra placements first is an
// explicit act.
//
// THE INVERSE RESTORES THE ORIGINALS (QBX-284, the author's decision of
// 2026-11-05; mixer CONTRACT inv. 15). This action PINS the arrangement root,
// every asset over it, and the master lane its placement sat on when the
// dissolve removes that lane, and SRestoreDissolvedArrangementAction
// re-registers THOSE objects and moves the tracks back into THAT root. It used
// to rebuild through extract-arrangement, which made a fresh root (its master
// lane's inserts, fader and automation lost), re-made only the asset named
// after the arrangement (any other asset lost), always placed it (an unplaced
// asset came back placed), refused an arrangement with no tracks, and left a
// restore-plan property a created arrangement never had.
//
// THE PINS ARE HELD ONLY WHILE THEY ARE IN USE: taken by apply(), dropped by
// the inverse once the registries and the master hold their own references
// again (a redo re-pins), and dropped by the destructor when the command is
// discarded in the dissolved state. Held on THIS action rather than on the
// inverse because the undo command reuses this object for redo
// (SRemoveArrangementAction's shape). Nothing in the model can reach an
// action, so a pin here cannot close a refcount cycle. The moved TRACKS are
// held weakly (QPointer): the master owns them while the arrangement is
// dissolved, and the inverse refuses if one has gone.
class SDissolveArrangementAction : public SAction {
public:
    SDissolveArrangementAction() = default;
    explicit SDissolveArrangementAction( const QString &name,
                                         const QString &restorePlan = QString() );
    ~SDissolveArrangementAction() override;

    QString name() const override { return QStringLiteral("dissolve-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("name"), QStringLiteral("restorePlan") }; }

    // What the dissolve took apart, for the inverse. Valid while holdsPins().
    struct Held {
        SObject *root = nullptr;                    // pinned
        QList<QPair<QString, SObject *> > assets;   // pinned, registry order
        QList<QPointer<STrack> > tracks;            // weak, in the root's order
        bool     placed = false;                    // was there a placement?
        SObject *placedBody = nullptr;              // one of `assets`
        QPointer<SObject> placementLane;            // weak unless laneRemoved
        bool     laneRemoved = false;               // lane pinned in `lane`
        STrack  *lane = nullptr;                    // pinned when laneRemoved
        int      laneIndex = -1;                    // its master index
        int      placementIndex = -1;               // the link's index in the lane
        offset_t startTime = 0;                     // the link's own fields
        Fraction startTicks;
        SLink::Timebase timebase = SLink::Timebase::Time;
        int      eventChannelOverride = -1;
        QVariant plan;                              // restorePlan property before
    };
    bool holdsPins() const { return holds_; }
    const Held &held() const { return held_; }
    /** Called by the inverse once the registries and the master hold their
     *  own references again. */
    void releaseHeld();
    /** extract-arrangement builds its inverse with this false: that command's
     *  redo re-extracts, so nothing the dissolve takes apart is ever restored
     *  and a pin would only keep a dead root alive (pin only while in use). */
    void setPinsForUndo( bool on ) { pinsForUndo_ = on; }

private:
    void dropPins();

    QString arrName_;
    QString restorePlan_;
    Held    held_;
    bool    holds_ = false;
    bool    pinsForUndo_ = true;
};

// The inverse half. Not a scriptable verb: it exists only on the undo stack and
// addresses what it restores by POINTER, through the dissolve that pinned it,
// which is the whole point -- a name would rebuild, and rebuilding is what
// lost the master lane's state and every asset not named after the
// arrangement.
class SRestoreDissolvedArrangementAction : public SAction {
public:
    SRestoreDissolvedArrangementAction( SDissolveArrangementAction *owner,
                                        const QString &name,
                                        const QString &restorePlan );

    QString name() const override { return QStringLiteral("restore-dissolved-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    SDissolveArrangementAction *owner_;   // holds the pins (not owned)
    QString name_;
    QString restorePlan_;
};

#endif // SDISSOLVEARRANGEMENTACTION_H
