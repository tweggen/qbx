#include "app/objects/mixer/sremovearrangementaction.h"
#include "app/objects/mixer/screatearrangementaction.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

SRemoveArrangementAction::SRemoveArrangementAction( const QString &name )
    : name_( name )
{
}

SRemoveArrangementAction::~SRemoveArrangementAction()
{
    dropStalePin();
}

void SRemoveArrangementAction::dropStalePin()
{
    if( holdsRef_ && heldRoot_ ) heldRoot_->removeRef();
    heldRoot_ = nullptr;
    holdsRef_ = false;
}

void SRemoveArrangementAction::releaseHeld()
{
    dropStalePin();
}

SApplyResult SRemoveArrangementAction::apply( SProject *project )
{
    if( !project || name_.isEmpty() ) {
        return { false, nullptr };
    }
    SObject *root = project->arrangement( name_ );
    if( !root ) {
        return { false, nullptr };
    }

    // Refuse while an asset windows this root. See the header: dropping the pin
    // under a live asset leaves a window onto a container that is about to die,
    // and no inverse can rebuild what it contained.
    for( const QString &assetName : project->assetNames() ) {
        SObject *body = project->asset( assetName );
        SCut *cut = dynamic_cast<SCut *>( body );
        if( cut && &cut->getContent() == root ) {
            return { false, nullptr };
        }
    }

    // Refuse a non-empty root as well. The inverse rebuilds an EMPTY mixer, so
    // dropping one that still holds lanes would lose them on undo — silently,
    // which is the failure mode this whole proposal exists to avoid.
    if( root->childCount() > 0 ) {
        return { false, nullptr };
    }

    // PIN THE ROOT BEFORE UNREGISTERING. unregisterArrangement() drops the
    // registry's reference, which "may reach 0 -> deleteLater"; without a
    // reference of our own the object is gone and the inverse could only build
    // a new one. Held on THIS action rather than on the inverse, because the
    // undo command reuses this object and the pin has to survive a redo --
    // SRemoveTrackAction's own reason, and the same shape.
    dropStalePin();
    root->addRef();
    heldRoot_ = root;
    holdsRef_ = true;

    project->unregisterArrangement( name_ );

    return { true, new SRestoreArrangementAction( this, name_ ) };
}

void SRemoveArrangementAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "name", name_ );
}

bool SRemoveArrangementAction::readXml( const QDomElement &elem, int /*version*/ )
{
    name_ = elem.attribute( "name" );
    return true;
}

static const bool s_reg_removearrangement = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-arrangement"),
        []{ return new SRemoveArrangementAction; }
    ), true
);

// --- restore-arrangement ----------------------------------------------------

SRestoreArrangementAction::SRestoreArrangementAction( SRemoveArrangementAction *owner,
                                                      const QString &name )
    : owner_( owner ), name_( name )
{
}

SApplyResult SRestoreArrangementAction::apply( SProject *project )
{
    if( !project || !owner_ || name_.isEmpty() ) return { false, nullptr };
    SObject *root = owner_->heldRoot();
    if( !root ) return { false, nullptr };
    // A name that came back while we were undone is not ours to overwrite.
    if( project->hasArrangement( name_ ) ) return { false, nullptr };

    project->registerArrangement( name_, root );
    // registerArrangement takes the registry's OWN reference, so ours has to
    // go: holding both would make the arrangement undestroyable for the rest
    // of the session. A redo re-pins through SRemoveArrangementAction::apply.
    owner_->releaseHeld();

    return { true, new SRemoveArrangementAction( name_ ) };
}

void SRestoreArrangementAction::writeXml( QDomElement &elem ) const
{
    // Undo-stack only: it addresses its root by pointer, so there is nothing
    // portable to write and nothing a script could re-create from a file.
    elem.setAttribute( "name", name_ );
}

bool SRestoreArrangementAction::readXml( const QDomElement &elem, int )
{
    name_ = elem.attribute( "name" );
    return true;
}
