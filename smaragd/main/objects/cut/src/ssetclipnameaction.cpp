#include "app/objects/cut/ssetclipnameaction.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/stakestack.h"
#include "app/model/seditgroups.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include <QDomElement>

using namespace strackpath;

namespace {
// Same take-resolution rule as set-pitch / set-formant-preserve: the
// placement itself, or the ONE take being edited on a stack (reported so the
// inverse can name it instead of re-resolving against a possibly different
// active take).
SClipWindow *clipNameTargetWindow( SObject *root, const QList<int> &clipPath,
                                   int take, int &resolvedTake )
{
    resolvedTake = -1;
    SLink *link = splacements::placementAt( root, clipPath );
    if( !link ) return nullptr;

    if( STakeStack *stack = dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
        resolvedTake = ( take >= 0 ) ? take : stack->activeTakeIndex();
        return stack->takeAt( resolvedTake );
    }
    return SClipWindow::of( &link->getSObject() );
}
}

SSetClipNameAction::SSetClipNameAction(
        const QList<int> &clipPath, const QString &name, int take,
        bool broadcast )
    : clipPath_( clipPath ), name_( name ), take_( take ),
      broadcast_( broadcast )
{
}

SApplyResult SSetClipNameAction::apply( SProject *project )
{
    if( !project || clipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) {
        return {false, nullptr};
    }

    // Edit-group broadcast, mirroring set-pitch.
    if( broadcast_ ) {
        QList<QList<int>> targets =
            seditgroups::expandClipPaths( project, clipPath_ );
        if( targets.size() > 1 ) {
            int t = take_;
            if( t < 0 ) {
                int resolved = -1;
                clipNameTargetWindow( mixer, clipPath_, -1, resolved );
                t = resolved;
            }
            SCompositeAction composite;
            for( const QList<int> &p : targets ) {
                int resolved = -1;
                if( !clipNameTargetWindow( mixer, p, t, resolved ) ) continue;
                composite.append(
                    new SSetClipNameAction( p, name_, t, false ) );
            }
            if( composite.count() > 0 )
                return composite.apply( project );
            return {false, nullptr};
        }
    }

    int resolvedTake = -1;
    SClipWindow *win = clipNameTargetWindow( mixer, clipPath_, take_,
                                             resolvedTake );
    if( !win ) {
        return {false, nullptr};
    }

    // The name is the SObject's — every window is one (SClipWindow::asObject).
    const QString oldName = win->asObject().getSName();
    win->asObject().setSName( name_ );

    // The inverse carries the OLD name and the RESOLVED take with broadcast
    // off: the group members each pushed their own inverse already, and the
    // active take may move before undo runs.
    SAction *inverse = new SSetClipNameAction( clipPath_, oldName,
                                               resolvedTake, false );
    return {true, inverse};
}

void SSetClipNameAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", pathToString( clipPath_ ) );
    elem.setAttribute( "name", name_ );
    elem.setAttribute( "take", take_ );
    elem.setAttribute( "broadcast", broadcast_ ? 1 : 0 );
}

bool SSetClipNameAction::readXml( const QDomElement &elem, int )
{
    clipPath_  = stringToPath( elem.attribute( "clip" ) );
    name_      = elem.attribute( "name" );
    take_      = elem.attribute( "take", "-1" ).toInt();
    broadcast_ = elem.attribute( "broadcast", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_setclipname = (
    SActionRegistry::instance().registerType(
        QStringLiteral("set-clip-name"),
        []{ return new SSetClipNameAction; }
    ), true
);
