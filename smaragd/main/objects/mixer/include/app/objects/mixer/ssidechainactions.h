#ifndef SSIDECHAINACTIONS_H
#define SSIDECHAINACTIONS_H

#include "app/actions/saction.h"

#include <QPointer>

class SObject;

/**
 * `set-plugin-sidechain` / `clear-plugin-sidechain` -- proposal 55 M4 (D5,
 * D6, D7's key edge, D8 `<sidechain>`, D9).
 *
 * A KEYED SLOT names a key lane and a tap point (`pre` = after the lane's
 * inserts, before its fader; `post` = after the fader; default `pre`, so
 * pulling the key lane's fader down does not stop the ducking). These verbs
 * edit that attribute (STrack::setPluginSlotSidechain) and nothing else; the
 * WIRING of the insert's port 1 is the mixer pass's (SStdMixer::rewireKeys,
 * 47 D4), which the setter asks to run.
 *
 * Verbs take `trackPath` and `slot` as every plugin verb does, and `source`
 * as any lane spelling (`$send:<name>` included, 45 D9); the FILE stores
 * identity (D8): `<sidechain source='<objectId>' tap='pre'/>` in the slot.
 *
 * THE REFUSALS, each announced with a message naming both ends and worded so
 * that only that refusal produces it:
 *
 *  - a slot that is empty, not loaded, DualMono (D9: "the plugin's
 *    sidechain" has one candidate per channel), or whose plugin has no
 *    sidechain input bus;
 *  - the MASTER lane as a key -- its output is the sum that already contains
 *    every lane; the conductor lane -- it carries no audio;
 *  - a self edge -- a track keying its own slot;
 *  - a cycle over the union of edge kinds (D7): keyed from its own folder
 *    (the containment-only shape), a return's source keyed from the return,
 *    a send loop closed by a key;
 *  - a source in another arrangement than the keyed track;
 *  - a `tap` other than `pre` or `post`.
 *
 * THE KEY IGNORES MUTE AND SOLO (D6), so nothing here or in the pass asks the
 * audibility rule. The LOADER does not come through here, so the pass fails
 * closed on whatever a file carries (dangling source, cycle) and announces it.
 */
class SSetPluginSidechainAction : public SAction {
public:
    SSetPluginSidechainAction() = default;
    SSetPluginSidechainAction( const QString &trackPath, int slot,
                               const QString &source, const QString &tap = QString() );

    QString name() const override { return QStringLiteral( "set-plugin-sidechain" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "slot" ),
               QStringLiteral( "source" ), QStringLiteral( "tap" ) }; }

private:
    QString trackPath_;     ///< the KEYED track
    int     slot_ = 0;
    QString source_;        ///< the key lane
    QString tap_;           ///< "pre" (default, written only when given) | "post"
};

class SClearPluginSidechainAction : public SAction {
public:
    SClearPluginSidechainAction() = default;
    SClearPluginSidechainAction( const QString &trackPath, int slot );

    QString name() const override { return QStringLiteral( "clear-plugin-sidechain" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "slot" ) }; }

private:
    QString trackPath_;
    int     slot_ = 0;
};

/**
 * THE INVERSE of both verbs: put back exactly the sidechain slot `slot` had
 * -- set or not, its source by OBJECT (a lane removed meanwhile is alive on
 * the undo stack and no path reaches it), its tap point. The slot by INDEX is
 * safe here because an inverse runs in LIFO order, so every edit after the
 * forward verb has already been undone. Not registered and not scriptable:
 * it is built live from the state a forward verb found, like
 * restore-return-source.
 */
class SRestorePluginSidechainAction : public SAction {
public:
    SRestorePluginSidechainAction( const QString &trackSpec, int slot, bool set,
                                   SObject *source, bool preFader );

    QString name() const override { return QStringLiteral( "restore-plugin-sidechain" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &, int ) override { return false; }

private:
    QString           trackSpec_;
    int               slot_ = 0;
    bool              set_  = false;
    QPointer<SObject> source_;
    bool              pre_  = true;
};

#endif
