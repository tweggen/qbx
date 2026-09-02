#include "app/testkit/sassertsystemlaneaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/spluginchain.h"

SApplyResult SAssertSystemLaneAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    // Resolve through the ORDINARY path machinery, deliberately: what is being
    // gated is that "$master" reaches the lane the same way "0" reaches a user
    // track, not that some test-only accessor can find it.
    const strackpath::QualifiedPath q = strackpath::parseQualified( trackPath_ );
    SObject *root = splacements::rootNamed( project, q.root );
    if( !root ) {
        qWarning() << "assert-system-lane: no root for" << trackPath_;
        return { false, nullptr };
    }
    SObject *obj = strackpath::resolveByPath( root, q.idx );
    if( !obj ) {
        qWarning() << "assert-system-lane FAILED:" << trackPath_
                   << "resolves to nothing";
        return { false, nullptr };
    }

    bool ok = true;
    auto fail = [&]( const QString &what, const QString &got,
                     const QString &want ) {
        qWarning().noquote() << "assert-system-lane FAILED [" << trackPath_
                             << "]" << what << ": got" << got << "want" << want;
        ok = false;
    };

    const QString gotRole =
        QString::fromLatin1( systemRoleToString( obj->systemRole() ) );
    if( gotRole != role_ ) fail( "role", gotRole, role_ );

    if( !hidden_.isEmpty() ) {
        const bool want = hidden_.startsWith( '1' ) || hidden_.startsWith( 't' );
        if( obj->isHidden() != want )
            fail( "hidden", obj->isHidden() ? "1" : "0", want ? "1" : "0" );
    }

    if( !acceptsClips_.isEmpty() ) {
        const bool want = acceptsClips_.startsWith( '1' )
                          || acceptsClips_.startsWith( 't' );
        if( obj->acceptsClips() != want )
            fail( "acceptsClips", obj->acceptsClips() ? "1" : "0",
                  want ? "1" : "0" );
    }

    if( !name_.isEmpty() && obj->getSName() != name_ )
        fail( "name", obj->getSName(), name_ );

    if( !volume_.isEmpty() ) {
        const double want = volume_.toDouble();
        const double got  = obj->getVolume();
        // The fader is stored as the dB the verb wrote; an exact compare is
        // right here and a tolerance would hide a curve round-tripping wrong.
        if( qAbs( got - want ) > 1e-9 )
            fail( "volume", QString::number( got, 'g', 12 ),
                  QString::number( want, 'g', 12 ) );
    }

    if( plugins_ >= 0 ) {
        STrack *track = dynamic_cast<STrack *>( obj );
        SPluginChain *chain = track ? track->getPluginChain() : nullptr;
        const int got = chain ? chain->getSlotCount() : -1;
        if( got != plugins_ )
            fail( "plugins", QString::number( got ),
                  QString::number( plugins_ ) );
    }

    // THE WRITE SIDE (D9). pathOf() is what every head control derives its
    // commit address from; before the sentinel it answered {} for a system
    // lane, which is also "the root itself".
    if( !expectPath_.isEmpty() ) {
        const QString got =
            strackpath::pathToString( strackpath::pathOf( root, obj ) );
        if( got != expectPath_ )
            fail( "pathOf", got.isEmpty() ? QStringLiteral( "<empty = the root>" )
                                          : got,
                  expectPath_ );
    }

    // NOT a child link (D2): an index path must not be able to reach it, or
    // every path in every case and fixture would have shifted by one.
    if( !inChildLinks_.isEmpty() ) {
        const bool want = inChildLinks_.startsWith( '1' );
        bool got = false;
        for( SLink *lk : root->childLinks() )
            if( lk && &lk->getSObject() == obj ) { got = true; break; }
        if( got != want )
            fail( "inChildLinks", got ? "1" : "0", want ? "1" : "0" );
    }

    return { ok, nullptr };
}

void SAssertSystemLaneAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "role", role_ );
    if( !hidden_.isEmpty() )       elem.setAttribute( "hidden", hidden_ );
    if( !acceptsClips_.isEmpty() ) elem.setAttribute( "acceptsClips", acceptsClips_ );
    if( plugins_ >= 0 )            elem.setAttribute( "plugins", plugins_ );
    if( !volume_.isEmpty() )       elem.setAttribute( "volume", volume_ );
    if( !name_.isEmpty() )         elem.setAttribute( "name", name_ );
    if( !expectPath_.isEmpty() )   elem.setAttribute( "expectPath", expectPath_ );
    if( !inChildLinks_.isEmpty() ) elem.setAttribute( "inChildLinks", inChildLinks_ );
}

bool SAssertSystemLaneAction::readXml( const QDomElement &elem, int )
{
    trackPath_    = elem.attribute( "trackPath", "$master" );
    role_         = elem.attribute( "role", "master" );
    hidden_       = elem.attribute( "hidden" );
    acceptsClips_ = elem.attribute( "acceptsClips" );
    plugins_      = elem.attribute( "plugins", "-1" ).toInt();
    volume_       = elem.attribute( "volume" );
    name_         = elem.attribute( "name" );
    expectPath_   = elem.attribute( "expectPath" );
    inChildLinks_ = elem.attribute( "inChildLinks" );
    return true;
}

static const bool s_reg_assert_system_lane = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-system-lane" ),
        []{ return new SAssertSystemLaneAction; } ), true );
