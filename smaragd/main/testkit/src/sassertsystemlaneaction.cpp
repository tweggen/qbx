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
    // THROUGH laneBySpec (proposal 45 M7), which is the same peel-and-resolve
    // plus the `$send:<name>` spelling. A verb whose whole subject is system
    // lanes that could not use the system-lane spelling would be an odd gate.
    QString rootName;
    SObject *root = nullptr;
    SObject *obj = splacements::laneBySpec( project, trackPath_, rootName,
                                            &root );
    if( !root ) {
        qWarning() << "assert-system-lane: no root for" << trackPath_;
        return { false, nullptr };
    }
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
        // laneHidden(), NOT isHidden(), and the difference is not academic.
        // SObject::isHidden() reads the plain `hidden_` flag, which the master
        // lane's constructor sets explicitly and which NOTHING ELSE in the app
        // consults -- every other isHidden() in the tree is QWidget's.
        // laneHidden() is the one the row walk asks
        // (SStdMixerView::appendSystemRows / appendRowsFor), and it defaults to
        // laneHiddenByDefault(), i.e. true for every system lane. A conductor
        // lane (proposal 45 M6) is hidden by that DEFAULT and sets no flag, so
        // a verb reading the flag reported it visible while the arranger drew
        // no row for it. Ask what the view asks.
        const bool want = hidden_.startsWith( '1' ) || hidden_.startsWith( 't' );
        if( obj->laneHidden() != want )
            fail( "hidden", obj->laneHidden() ? "1" : "0", want ? "1" : "0" );
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

    // ---- THE AC5.5 OBSERVABLES (proposal 45 M5) ------------------------
    //
    // What an AC5.2 refusal must have left alone. Asserting only that the verb
    // returned false would pass for a verb that wrote its field and THEN
    // refused, which is exactly the half-application AC5.5 exists to forbid.
    auto wantBool = []( const QString &s ) {
        return s.startsWith( '1' ) || s.startsWith( 't' );
    };

    if( !armed_.isEmpty() && obj->isArmedForRecording() != wantBool( armed_ ) )
        fail( "armed", obj->isArmedForRecording() ? "1" : "0",
              wantBool( armed_ ) ? "1" : "0" );

    if( !solo_.isEmpty() && obj->isSolo() != wantBool( solo_ ) )
        fail( "solo", obj->isSolo() ? "1" : "0", wantBool( solo_ ) ? "1" : "0" );

    if( !muted_.isEmpty() && obj->isMuted() != wantBool( muted_ ) )
        fail( "muted", obj->isMuted() ? "1" : "0", wantBool( muted_ ) ? "1" : "0" );

    if( !monitor_.isEmpty() || !input_.isEmpty() || !midiOutPort_.isEmpty() ) {
        STrack *track = dynamic_cast<STrack *>( obj );
        if( !track ) {
            fail( "kind", "not an STrack",
                  "an STrack (monitor/input/midiOutPort were asked for)" );
        } else {
            if( !monitor_.isEmpty() ) {
                const QString got =
                    STrack::monitorModeToString( track->getMonitorMode() );
                if( got != monitor_ ) fail( "monitor", got, monitor_ );
            }
            if( !input_.isEmpty() ) {
                // "" and the literal "none" are the same state; the verb
                // spells the ABSENCE of an input as "none" so that a case can
                // assert it without writing an empty attribute, which means
                // "skip" everywhere else in this verb.
                QString got = track->getTrackInput();
                if( got.isEmpty() ) got = QStringLiteral( "none" );
                if( got != input_ ) fail( "input", got, input_ );
            }
            if( !midiOutPort_.isEmpty() ) {
                QString got = track->getMidiOutPort();
                if( got.isEmpty() ) got = QStringLiteral( "<none>" );
                if( got != midiOutPort_ ) fail( "midiOutPort", got, midiOutPort_ );
            }
        }
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
    if( !armed_.isEmpty() )        elem.setAttribute( "armed", armed_ );
    if( !monitor_.isEmpty() )      elem.setAttribute( "monitor", monitor_ );
    if( !input_.isEmpty() )        elem.setAttribute( "input", input_ );
    if( !solo_.isEmpty() )         elem.setAttribute( "solo", solo_ );
    if( !muted_.isEmpty() )        elem.setAttribute( "muted", muted_ );
    if( !midiOutPort_.isEmpty() )  elem.setAttribute( "midiOutPort", midiOutPort_ );
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
    armed_        = elem.attribute( "armed" );
    monitor_      = elem.attribute( "monitor" );
    input_        = elem.attribute( "input" );
    solo_         = elem.attribute( "solo" );
    muted_        = elem.attribute( "muted" );
    midiOutPort_  = elem.attribute( "midiOutPort" );
    return true;
}

static const bool s_reg_assert_system_lane = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-system-lane" ),
        []{ return new SAssertSystemLaneAction; } ), true );
