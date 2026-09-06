#include "app/objects/mixer/ssendtapactions.h"

#include <QDebug>
#include <QDomElement>
#include <QSet>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"

namespace {

struct Resolved {
    SStdMixer *mixer = nullptr;
    SObject   *source = nullptr;
    STrack    *dest = nullptr;
};

/**
 * THE ONE RESOLVER, so the three verbs cannot drift apart in what they refuse
 * or in what they say about it — the argument `splacements::refuseSystemLane`
 * already makes for proposal 45's eight structural verbs.
 *
 * `needDest` is false for `remove-send`: a tap whose destination lane has since
 * been deleted must still be REMOVABLE, or the only way to clear it would be to
 * re-create a lane of that name first.
 */
bool resolve( SProject *project, const QString &verb, const QString &track,
              const QString &dest, const QString &arrangement,
              bool needDest, Resolved &out )
{
    QString root = arrangement;
    SObject *rootObj = nullptr;
    out.source = splacements::laneBySpec( project, track, root, &rootObj );
    if( !out.source ) {
        qWarning().noquote()
            << verb << ": refused, no lane at" << track
            << "-- the source of a send is addressed like any other lane";
        return false;
    }
    out.mixer = dynamic_cast<SStdMixer *>( rootObj );
    if( !out.mixer ) {
        qWarning().noquote() << verb << ": refused, no mixer root for" << track;
        return false;
    }
    if( dest.trimmed().isEmpty() ) {
        qWarning().noquote()
            << verb << ": refused, a send needs a destination -- the NAME of a"
            << "send lane, which is its address ($send:<name>)";
        return false;
    }
    out.dest = out.mixer->sendLaneNamed( dest );
    if( needDest && !out.dest ) {
        qWarning().noquote()
            << verb << ": refused, there is no send lane called" << dest
            << "in this arrangement";
        return false;
    }
    return true;
}

/// True when `from` can already reach `to` by following existing taps, so
/// adding `to -> from` would close a cycle. The graph is at most
/// (lanes x sends) edges and is walked on an EDIT, never per page.
bool reaches( SStdMixer *mixer, SObject *from, const SObject *to )
{
    QSet<const SObject *> seen;
    QList<SObject *> work;
    work.append( from );
    while( !work.isEmpty() ) {
        SObject *cur = work.takeLast();
        if( !cur || seen.contains( cur ) ) continue;
        seen.insert( cur );
        if( cur == to ) return true;
        for( const SSendTap &t : cur->sendTaps() ) {
            if( STrack *next = mixer->sendLaneNamed( t.dest ) )
                work.append( static_cast<SObject *>( next ) );
        }
    }
    return false;
}

/// D6's refusals that only apply when a tap is being CREATED or re-pointed.
bool routingIsLegal( const QString &verb, const Resolved &r )
{
    if( r.dest->systemRole() != SSystemRole::Send ) {
        qWarning().noquote()
            << verb << ": refused, the destination is not a send lane";
        return false;
    }
    if( static_cast<SObject *>( r.dest ) == r.source ) {
        qWarning().noquote()
            << verb << ": refused, a send lane cannot send to itself";
        return false;
    }
    // The master's output IS the sum that already contains every send lane, so
    // master -> send closes a cycle whatever the tap graph looks like. Refused
    // by ROLE rather than left to the walk below, because the edge that closes
    // it is the mixer's own wiring and is not in the tap graph at all.
    if( r.source->systemRole() == SSystemRole::Master ) {
        qWarning().noquote()
            << verb << ": refused, the master lane cannot feed a send -- its"
            << "output is the sum that already contains every send lane";
        return false;
    }
    if( reaches( r.mixer, static_cast<SObject *>( r.dest ), r.source ) ) {
        qWarning().noquote()
            << verb << ": refused, this would create a send CYCLE --" << r.dest->getSName()
            << "already reaches" << r.source->getSName()
            << ". A cycle does not fail cleanly: the page scheduler would leave"
            << "two nodes each waiting on the other and the render would hang.";
        return false;
    }
    return true;
}

/// The inverse that restores exactly what `prev` was, or removes the tap when
/// there was none. Both verbs that replace a tap need it, so it is spelled once.
SAction *inverseFor( const QString &track, const QString &dest,
                     const QString &arrangement, const SSendTap *prev )
{
    if( !prev ) return new SRemoveSendAction( track, dest, arrangement );
    return new SAddSendAction( track, dest, prev->levelDb, prev->preFader,
                               prev->enabled, arrangement );
}

}  // namespace

// --- add-send ---------------------------------------------------------------

SAddSendAction::SAddSendAction( const QString &track, const QString &dest,
                                double levelDb, bool preFader, bool enabled,
                                const QString &arrangement )
    : track_( track ), dest_( dest ), levelDb_( levelDb ),
      preFader_( preFader ), enabled_( enabled ), arrangement_( arrangement )
{
}

SApplyResult SAddSendAction::apply( SProject *project )
{
    Resolved r;
    if( !resolve( project, name(), track_, dest_, arrangement_, true, r ) )
        return { false, nullptr };
    if( !routingIsLegal( name(), r ) ) return { false, nullptr };

    // Captured BEFORE the write: add-send on a destination that already has a
    // tap is a REPLACE (SObject::setSendTap), so its inverse has to restore
    // the old values rather than remove the tap.
    const SSendTap *existing = r.source->sendTap( dest_ );
    const SSendTap prev = existing ? *existing : SSendTap{};
    const bool had = existing != nullptr;

    SSendTap tap;
    tap.dest = dest_;
    tap.levelDb = levelDb_;
    tap.preFader = preFader_;
    tap.enabled = enabled_;
    if( !r.source->setSendTap( tap ) ) return { false, nullptr };

    return { true, inverseFor( track_, dest_, arrangement_,
                               had ? &prev : nullptr ) };
}

void SAddSendAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "dest", dest_ );
    elem.setAttribute( "level", levelDb_ );
    elem.setAttribute( "pre", preFader_ ? "true" : "false" );
    elem.setAttribute( "enabled", enabled_ ? "true" : "false" );
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
}

bool SAddSendAction::readXml( const QDomElement &elem, int )
{
    track_ = elem.attribute( "track" );
    dest_  = elem.attribute( "dest" );
    levelDb_ = elem.attribute( "level", "0" ).toDouble();
    preFader_ = elem.attribute( "pre", "false" ) == QLatin1String( "true" );
    enabled_  = elem.attribute( "enabled", "true" ) == QLatin1String( "true" );
    arrangement_ = elem.attribute( "arrangement" );
    return !track_.isEmpty() && !dest_.isEmpty();
}

static const bool s_reg_add_send = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "add-send" ), []{ return new SAddSendAction; } ), true );

// --- remove-send ------------------------------------------------------------

SRemoveSendAction::SRemoveSendAction( const QString &track, const QString &dest,
                                      const QString &arrangement )
    : track_( track ), dest_( dest ), arrangement_( arrangement )
{
}

SApplyResult SRemoveSendAction::apply( SProject *project )
{
    Resolved r;
    // needDest = false: a tap whose lane has since been removed must still be
    // clearable (see resolve()).
    if( !resolve( project, name(), track_, dest_, arrangement_, false, r ) )
        return { false, nullptr };

    const SSendTap *existing = r.source->sendTap( dest_ );
    if( !existing ) {
        qWarning().noquote()
            << name() << ": refused," << track_ << "has no send to" << dest_;
        return { false, nullptr };
    }
    const SSendTap prev = *existing;
    r.source->removeSendTap( dest_ );

    return { true, new SAddSendAction( track_, dest_, prev.levelDb,
                                       prev.preFader, prev.enabled,
                                       arrangement_ ) };
}

void SRemoveSendAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "dest", dest_ );
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
}

bool SRemoveSendAction::readXml( const QDomElement &elem, int )
{
    track_ = elem.attribute( "track" );
    dest_  = elem.attribute( "dest" );
    arrangement_ = elem.attribute( "arrangement" );
    return !track_.isEmpty() && !dest_.isEmpty();
}

static const bool s_reg_remove_send = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "remove-send" ), []{ return new SRemoveSendAction; } ), true );

// --- set-send ---------------------------------------------------------------

SSetSendAction::SSetSendAction( const QString &track, const QString &dest,
                                const QString &arrangement )
    : track_( track ), dest_( dest ), arrangement_( arrangement )
{
}

SApplyResult SSetSendAction::apply( SProject *project )
{
    Resolved r;
    if( !resolve( project, name(), track_, dest_, arrangement_, true, r ) )
        return { false, nullptr };

    const SSendTap *existing = r.source->sendTap( dest_ );
    if( !existing ) {
        qWarning().noquote()
            << name() << ": refused," << track_ << "has no send to" << dest_
            << "-- add-send creates one";
        return { false, nullptr };
    }
    if( !hasLevel_ && !hasPre_ && !hasEnabled_ ) {
        qWarning().noquote()
            << name() << ": refused, nothing to set (give level, pre or enabled)";
        return { false, nullptr };
    }

    const SSendTap prev = *existing;
    SSendTap tap = prev;
    if( hasLevel_ )   tap.levelDb  = levelDb_;
    if( hasPre_ )     tap.preFader = preFader_;
    if( hasEnabled_ ) tap.enabled  = enabled_;
    // Changing the MODE does not change the routing graph, so no cycle walk is
    // needed here: the destination is unchanged by construction.
    r.source->setSendTap( tap );

    return { true, new SAddSendAction( track_, dest_, prev.levelDb,
                                       prev.preFader, prev.enabled,
                                       arrangement_ ) };
}

void SSetSendAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "dest", dest_ );
    if( hasLevel_ )   elem.setAttribute( "level", levelDb_ );
    if( hasPre_ )     elem.setAttribute( "pre", preFader_ ? "true" : "false" );
    if( hasEnabled_ ) elem.setAttribute( "enabled", enabled_ ? "true" : "false" );
    if( !arrangement_.isEmpty() ) elem.setAttribute( "arrangement", arrangement_ );
}

bool SSetSendAction::readXml( const QDomElement &elem, int )
{
    track_ = elem.attribute( "track" );
    dest_  = elem.attribute( "dest" );
    arrangement_ = elem.attribute( "arrangement" );
    if( elem.hasAttribute( "level" ) )
        setLevelDb( elem.attribute( "level" ).toDouble() );
    if( elem.hasAttribute( "pre" ) )
        setPreFader( elem.attribute( "pre" ) == QLatin1String( "true" ) );
    if( elem.hasAttribute( "enabled" ) )
        setEnabled( elem.attribute( "enabled" ) == QLatin1String( "true" ) );
    return !track_.isEmpty() && !dest_.isEmpty();
}

static const bool s_reg_set_send = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-send" ), []{ return new SSetSendAction; } ), true );
