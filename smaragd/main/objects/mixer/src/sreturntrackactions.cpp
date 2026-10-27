#include "app/objects/mixer/sreturntrackactions.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sappcontext.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

namespace {

/// Every STrack in `root`'s lane tree, depth-first.
void collectTracks( SObject *container, QList<STrack *> &out )
{
    for( SLink *lk : container->childLinks() ) {
        if( !lk ) continue;
        if( STrack *t = dynamic_cast<STrack *>( &lk->getSObject() ) ) {
            out.append( t );
            collectTracks( t, out );
        }
    }
}

/// THE ONE SOURCE RESOLVER, so the three verbs refuse alike.
STrack *resolveTrack( SProject *project, const QString &verb, const QString &spec,
                      QString &rootInOut, SStdMixer **mixerOut, const char *what )
{
    SObject *rootObj = nullptr;
    SObject *obj = splacements::laneBySpec( project, spec, rootInOut, &rootObj );
    STrack *t = dynamic_cast<STrack *>( obj );
    if( !t ) {
        qWarning().noquote() << verb << ": refused, no track at" << spec
                             << "-- the" << what << "is addressed like any other lane";
        return nullptr;
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer *>( rootObj );
    if( !mixer ) {
        qWarning().noquote() << verb << ": refused, no mixer root for" << spec;
        return nullptr;
    }
    if( mixerOut ) *mixerOut = mixer;
    return t;
}

/**
 * D7 and D9's refusals for the edge `source -> ret` (or, for add-return-track,
 * a return about to be created under `newParent`). Each message names both
 * ends and is worded so that ONLY this refusal produces it (AC2.6).
 */
bool auxEdgeIsLegal( const QString &verb, SStdMixer *mixer, STrack *source,
                     int slot, int bus, STrack *ret, SObject *newParent )
{
    // The master's output IS the sum that already contains every return, so
    // master -> return closes a cycle whatever the graph says -- refused by
    // ROLE, as add-send refuses the master, because the edge that closes it is
    // the mixer's own wiring and is not in the routing graph.
    if( source->systemRole() == SSystemRole::Master ) {
        qWarning().noquote()
            << verb << ": refused, the MASTER lane cannot feed a return track --"
            << "its output is the sum that already contains every return";
        return false;
    }
    if( ret && ret == source ) {
        qWarning().noquote()
            << verb << ": refused," << source->getSName()
            << "cannot be its own return track (a self edge)";
        return false;
    }
    QString why;
    if( !source->auxBusRoutable( slot, bus, &why ) ) {
        qWarning().noquote()
            << verb << ": refused, cannot route" << source->getSName()
            << "slot" << slot << "bus" << bus << "--" << why;
        return false;
    }

    // ONE RETURN PER BUS (D9, Q4 at the proposal's default).
    const quint64 id = source->pluginSlotId( slot, false );
    if( id != 0 ) {
        QList<STrack *> tracks;
        collectTracks( mixer, tracks );
        for( STrack *t : tracks ) {
            if( t == ret || !t->isReturnTrack() ) continue;
            if( t->auxSourceTrack() == source && t->auxSourceSlotId() == id
                && t->auxSourceBus() == bus ) {
                qWarning().noquote()
                    << verb << ": refused, a second return of the same bus --"
                    << t->getSName() << "already returns bus" << bus << "of slot"
                    << slot << "on" << source->getSName()
                    << "(one return per bus; process it in parallel with a send"
                    << "from that return, proposal 55 D9)";
                return false;
            }
        }
    }

    // THE CYCLE WALK over the union of edge kinds (D7), the same sroute::Graph
    // the pass breaks cycles with.
    const sroute::Graph g = mixer->routingGraph();
    const auto node = []( const SObject *o ) { return static_cast<sroute::Node>( o ); };
    bool cycle = false;
    QString reaches;
    if( ret ) {
        cycle = g.wouldCloseCycle( node( source ), node( ret ) );
        reaches = ret->getSName();
    } else if( newParent && newParent != static_cast<SObject *>( mixer ) ) {
        // The new return's only edge out is containment into newParent, so the
        // aux edge closes a cycle exactly when newParent already reaches the
        // source -- "a return placed inside its own source" is the
        // containment-only shape of it (source -> return -> source).
        cycle = newParent == static_cast<SObject *>( source )
             || g.reaches( node( newParent ), node( source ) );
        reaches = newParent->getSName();
    }
    if( cycle ) {
        qWarning().noquote()
            << verb << ": refused, this would close a routing CYCLE --"
            << reaches << "already reaches" << source->getSName()
            << "(proposal 55 D7)";
        return false;
    }
    return true;
}

/// A track that is about to become a return must carry no clips and no child
/// tracks (D1) and must be a user track.
bool mayBecomeReturn( const QString &verb, STrack *t )
{
    if( t->isSystemLane() ) {
        qWarning().noquote()
            << verb << ": refused," << t->getSName()
            << "is a system lane; a return track is a user track (proposal 55 D1)";
        return false;
    }
    bool clips = false, kids = false;
    for( SLink *lk : t->childLinks() ) {
        if( !lk ) continue;
        if( dynamic_cast<STrack *>( &lk->getSObject() ) ) kids = true;
        else clips = true;
    }
    if( kids ) {
        qWarning().noquote()
            << verb << ": refused," << t->getSName()
            << "has child tracks, and a return track takes none (proposal 55 D1)";
        return false;
    }
    if( clips ) {
        qWarning().noquote()
            << verb << ": refused," << t->getSName()
            << "carries clips, and a return track carries none (proposal 55 D1)";
        return false;
    }
    return true;
}

}  // namespace

// --- add-return-track -------------------------------------------------------

SAddReturnTrackAction::SAddReturnTrackAction( const QString &trackPath, int slot,
                                              int bus, const QString &trackName,
                                              int at )
    : trackPath_( trackPath ), slot_( slot ), bus_( bus ),
      trackName_( trackName ), at_( at )
{
}

SApplyResult SAddReturnTrackAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SStdMixer *mixer = nullptr;
    STrack *source = resolveTrack( project, name(), trackPath_, pathRoot_, &mixer,
                                   "source of a return" );
    if( !source ) return { false, nullptr };

    // "Right after the source, at the same level" (§3.2): the source's parent
    // and its index there. A send lane's path is a sentinel; its parent is the
    // root, where the return is appended.
    SObject *root = static_cast<SObject *>( mixer );
    const QList<int> srcPath = strackpath::pathOf( root, source );
    if( srcPath.isEmpty() ) {
        qWarning().noquote() << name() << ": refused, the source is not in this arrangement";
        return { false, nullptr };
    }
    QList<int> parentPath = srcPath;
    const int srcIndex = parentPath.takeLast();
    SObject *parent = strackpath::spathIsSystemStep( srcIndex )
                      ? root : splacements::laneAt( root, parentPath );
    if( strackpath::spathIsSystemStep( srcIndex ) ) parentPath.clear();
    if( !parent ) return { false, nullptr };

    if( !auxEdgeIsLegal( name(), mixer, source, slot_, bus_, nullptr, parent ) )
        return { false, nullptr };

    STrack *ret = new STrack( project );
    ret->setSName( trackName_.isEmpty()
                   ? QStringLiteral( "%1 Aux %2" ).arg( source->getSName() ).arg( bus_ )
                   : trackName_ );

    int index = -1;
    if( SStdMixer *pm = dynamic_cast<SStdMixer *>( parent ) ) {
        pm->insertTrack( *ret );
        const int landing = pm->getNTracks() - 1;
        index = at_ >= 0 ? at_
              : strackpath::spathIsSystemStep( srcIndex ) ? landing : srcIndex + 1;
        if( index > landing ) index = landing;
        if( index != landing ) pm->reorderTrack( landing, index );
    } else if( STrack *pt = dynamic_cast<STrack *>( parent ) ) {
        // SLink ctor rule: no parent at construction, then setParent.
        SLink *lk = new SLink( *ret, nullptr );
        lk->setParent( pt );
        const int landing = pt->childCount() - 1;
        index = at_ >= 0 ? at_ : srcIndex + 1;
        if( index > landing ) index = landing;
        if( index != landing ) pt->moveChildToIndex( landing, index );
        mixer->notifyTreeChanged();
    } else {
        delete ret;
        return { false, nullptr };
    }

    // The ROUTE, by identity (D8). setAuxSource asks the root to re-run its
    // wiring pass, which builds the tap and hands the slot its mask.
    ret->setAuxSource( source, source->pluginSlotId( slot_, true ), bus_ );
    SAppContext::get().rewireSpeaker();

    QList<int> retPath = parentPath;
    retPath.append( index );
    return { true, new SRemoveTrackAction( retPath ) };
}

void SAddReturnTrackAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slot", slot_ );
    elem.setAttribute( "bus", bus_ );
    if( !trackName_.isEmpty() ) elem.setAttribute( "name", trackName_ );
    if( at_ >= 0 ) elem.setAttribute( "at", at_ );
}

bool SAddReturnTrackAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    slot_      = elem.attribute( "slot", "0" ).toInt();
    bus_       = elem.attribute( "bus", "1" ).toInt();
    trackName_ = elem.attribute( "name" );
    at_        = elem.attribute( "at", "-1" ).toInt();
    return !trackPath_.isEmpty();
}

static const bool s_reg_add_return_track = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-return-track" ), []{ return new SAddReturnTrackAction; } ),
    true );

// --- set-return-source ------------------------------------------------------

SSetReturnSourceAction::SSetReturnSourceAction( const QString &trackPath,
                                                const QString &sourcePath,
                                                int slot, int bus )
    : trackPath_( trackPath ), sourcePath_( sourcePath ), slot_( slot ), bus_( bus )
{
}

SApplyResult SSetReturnSourceAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SStdMixer *mixer = nullptr;
    STrack *ret = resolveTrack( project, name(), trackPath_, pathRoot_, &mixer,
                                "return track" );
    if( !ret ) return { false, nullptr };
    QString srcRoot = pathRoot_;
    SStdMixer *srcMixer = nullptr;
    STrack *source = resolveTrack( project, name(), sourcePath_, srcRoot, &srcMixer,
                                   "source of a return" );
    if( !source ) return { false, nullptr };
    if( srcMixer != mixer ) {
        qWarning().noquote()
            << name() << ": refused, the source" << sourcePath_
            << "is in another arrangement than the return" << trackPath_;
        return { false, nullptr };
    }
    if( !mayBecomeReturn( name(), ret ) ) return { false, nullptr };
    if( !auxEdgeIsLegal( name(), mixer, source, slot_, bus_, ret, nullptr ) )
        return { false, nullptr };

    SAction *inverse = new SRestoreReturnSourceAction(
        splacements::specOf( project, ret ), ret->auxSourceTrack(),
        ret->auxSourceSlotId(), ret->auxSourceBus() );
    ret->setAuxSource( source, source->pluginSlotId( slot_, true ), bus_ );
    return { true, inverse };
}

void SSetReturnSourceAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "sourcePath", sourcePath_ );
    elem.setAttribute( "slot", slot_ );
    elem.setAttribute( "bus", bus_ );
}

bool SSetReturnSourceAction::readXml( const QDomElement &elem, int )
{
    trackPath_  = elem.attribute( "trackPath" );
    sourcePath_ = elem.attribute( "sourcePath" );
    slot_       = elem.attribute( "slot", "0" ).toInt();
    bus_        = elem.attribute( "bus", "1" ).toInt();
    return !trackPath_.isEmpty() && !sourcePath_.isEmpty();
}

static const bool s_reg_set_return_source = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-return-source" ), []{ return new SSetReturnSourceAction; } ),
    true );

// --- clear-return-source ----------------------------------------------------

SClearReturnSourceAction::SClearReturnSourceAction( const QString &trackPath )
    : trackPath_( trackPath )
{
}

SApplyResult SClearReturnSourceAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    STrack *ret = resolveTrack( project, name(), trackPath_, pathRoot_, nullptr,
                                "return track" );
    if( !ret ) return { false, nullptr };
    if( !ret->isReturnTrack() ) {
        qWarning().noquote()
            << name() << ": refused," << ret->getSName() << "is not a return track";
        return { false, nullptr };
    }
    SAction *inverse = new SRestoreReturnSourceAction(
        splacements::specOf( project, ret ), ret->auxSourceTrack(),
        ret->auxSourceSlotId(), ret->auxSourceBus() );
    ret->clearAuxSource();
    return { true, inverse };
}

void SClearReturnSourceAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
}

bool SClearReturnSourceAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath" );
    return !trackPath_.isEmpty();
}

static const bool s_reg_clear_return_source = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "clear-return-source" ), []{ return new SClearReturnSourceAction; } ),
    true );

// --- the inverse ------------------------------------------------------------

SRestoreReturnSourceAction::SRestoreReturnSourceAction( const QString &trackSpec,
                                                        STrack *source,
                                                        quint64 slotId, int bus )
    : trackSpec_( trackSpec ), source_( source ), slotId_( slotId ), bus_( bus )
{
}

SApplyResult SRestoreReturnSourceAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    QString root;
    STrack *ret = dynamic_cast<STrack *>(
        splacements::laneBySpec( project, trackSpec_, root ) );
    if( !ret ) return { false, nullptr };
    SAction *inverse = new SRestoreReturnSourceAction(
        trackSpec_, ret->auxSourceTrack(), ret->auxSourceSlotId(), ret->auxSourceBus() );
    ret->setAuxSource( source_.data(), slotId_, bus_ );
    return { true, inverse };
}

void SRestoreReturnSourceAction::writeXml( QDomElement &elem ) const
{
    // Never serialized standalone (built live as an inverse); recorded for
    // completeness, as restore-track does.
    elem.setAttribute( "trackPath", trackSpec_ );
    elem.setAttribute( "slotId", QString::number( slotId_ ) );
    elem.setAttribute( "bus", bus_ );
}
