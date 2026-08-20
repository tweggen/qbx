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

    project->unregisterArrangement( name_ );

    return { true, new SCreateArrangementAction( name_ ) };
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
