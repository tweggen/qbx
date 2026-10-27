#ifndef SRETURNTRACKACTIONS_H
#define SRETURNTRACKACTIONS_H

#include "app/actions/saction.h"

#include <QPointer>
#include <QtGlobal>

#include "app/objects/track/strack.h"

/**
 * `add-return-track` / `set-return-source` / `clear-return-source` --
 * proposal 55 M2 (D1, D3, D8, D9).
 *
 * A RETURN TRACK is an ordinary user track whose input is ONE aux output bus
 * of ONE plugin slot: `(source track, slot, bus)`. These verbs edit that
 * attribute (STrack::setAuxSource) and nothing else; the WIRING is the mixer
 * pass's (SStdMixer::rewireReturns, 47 D4), which setAuxSource asks to run.
 *
 * Verbs take `trackPath` and `slot` as every plugin verb does; the FILE stores
 * identity (D8): the track as an object id, the slot as its stable slotId.
 *
 * THE REFUSALS (D7, D9 and §3.2), each ANNOUNCED with a message naming both
 * ends and worded so that only that refusal produces it (AC2.6):
 *
 *  - bus 0 -- the main bus is the track's own output;
 *  - a bus the plugin does not have, or a slot that is empty or not loaded;
 *  - a DualMono slot -- "bus k" has one candidate per channel;
 *  - a second return of the same (slot, bus) -- Q4 at its default, refused,
 *    naming the first; parallel processing of a bus is a send from its return;
 *  - the master lane as a source -- its output already contains every return;
 *  - a self edge -- a track as its own return;
 *  - a cycle over the union of edge kinds (D7), including the containment-only
 *    one "a return placed inside its own source";
 *  - set-return-source on a track that carries clips or child tracks, or on a
 *    system lane.
 *
 * The LOADER does not come through here, so the pass fails closed on whatever
 * a file carries (dangling source, cycle, duplicate) and announces it.
 */
class SAddReturnTrackAction : public SAction {
public:
    SAddReturnTrackAction() = default;
    SAddReturnTrackAction( const QString &trackPath, int slot, int bus,
                           const QString &trackName = QString(), int at = -1 );

    QString name() const override { return QStringLiteral( "add-return-track" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "slot" ),
               QStringLiteral( "bus" ), QStringLiteral( "name" ),
               QStringLiteral( "at" ) }; }

private:
    QString trackPath_;     ///< the SOURCE track
    int     slot_ = 0;
    int     bus_  = 1;
    QString trackName_;     ///< empty = "<source> Aux <bus>"
    int     at_   = -1;     ///< index in the source's parent; -1 = right after it
};

class SSetReturnSourceAction : public SAction {
public:
    SSetReturnSourceAction() = default;
    SSetReturnSourceAction( const QString &trackPath, const QString &sourcePath,
                            int slot, int bus );

    QString name() const override { return QStringLiteral( "set-return-source" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ), QStringLiteral( "sourcePath" ),
               QStringLiteral( "slot" ), QStringLiteral( "bus" ) }; }

private:
    QString trackPath_;     ///< the track that becomes (or is) the return
    QString sourcePath_;
    int     slot_ = 0;
    int     bus_  = 1;
};

class SClearReturnSourceAction : public SAction {
public:
    SClearReturnSourceAction() = default;
    explicit SClearReturnSourceAction( const QString &trackPath );

    QString name() const override { return QStringLiteral( "clear-return-source" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "trackPath" ) }; }

private:
    QString trackPath_;
};

/**
 * THE INVERSE of set-return-source and clear-return-source: put back exactly
 * the aux source a track had -- by slot ID, not by index, because the verb's
 * slot index is resolved at apply time and the inverse must name the same
 * slot whatever moved since (D8). bus 0 = "was not a return". Not registered and not scriptable: it is
 * built live from the state a forward verb found, like restore-track.
 */
class SRestoreReturnSourceAction : public SAction {
public:
    SRestoreReturnSourceAction( const QString &trackSpec, STrack *source,
                                quint64 slotId, int bus );

    QString name() const override { return QStringLiteral( "restore-return-source" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &, int ) override { return false; }

private:
    QString trackSpec_;
    // The source by OBJECT, not by spelling: it may be a track that has left
    // every root (removed, alive on the undo stack), which no path reaches.
    QPointer<STrack> source_;
    quint64 slotId_ = 0;
    int     bus_    = 0;
};

#endif
