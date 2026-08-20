#include "app/objects/mixer/screatearrangementaction.h"
#include "app/objects/mixer/sremovearrangementaction.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>

// First-unused "Arrangement N", so creating several in a row cannot collide and
// undo/redo can reuse an explicit name. Mirrors generateAssetName().
static QString generateArrangementName( SProject *project )
{
    for( int n = 1; ; ++n ) {
        QString candidate = QString( "Arrangement %1" ).arg( n );
        if( !project->hasArrangement( candidate ) ) return candidate;
    }
}

SCreateArrangementAction::SCreateArrangementAction( const QString &name )
    : name_( name )
{
}

SApplyResult SCreateArrangementAction::apply( SProject *project )
{
    if( !project ) {
        return { false, nullptr };
    }
    const QString name = name_.isEmpty() ? generateArrangementName( project )
                                         : name_;
    if( project->hasArrangement( name ) ) {
        return { false, nullptr };          // names are unique
    }

    // A second mixer, built exactly like the master's (proposal 09 D2): the
    // detail-editor factory is keyed on the CLASS NAME, so this root gets the
    // ordinary SStdMixerView with no change to the arranger, and the ctor reads
    // its channel width from the project and follows channelsChanged per
    // instance.
    //
    // Deliberately NOT inserted anywhere: it is a ROOT. registerArrangement()
    // takes the only reference it will have until an asset windows it.
    SStdMixer *root = new SStdMixer( project );
    root->setSName( name );
    project->registerArrangement( name, root );

    return { true, new SRemoveArrangementAction( name ) };
}

void SCreateArrangementAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "name", name_ );
}

bool SCreateArrangementAction::readXml( const QDomElement &elem, int /*version*/ )
{
    name_ = elem.attribute( "name" );
    return true;
}

static const bool s_reg_createarrangement = (
    SActionRegistry::instance().registerType(
        QStringLiteral("create-arrangement"),
        []{ return new SCreateArrangementAction; }
    ), true
);
