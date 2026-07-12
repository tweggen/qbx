
#include <stdlib.h>

#include <qfile.h>
#include <QDebug>

#include <iostream>

using namespace std;

#include "app/persistence/sprojectloader.h"

#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"

// For the factory stuff.


QHash<QString, SProjectLoader::instantiateFromDomElement_f> &
SProjectLoader::sObjectRegistry()
{
    static QHash<QString, instantiateFromDomElement_f> registry;
    return registry;
}

void SProjectLoader::registerSObjectClass(
    const QString &name, SProjectLoader::instantiateFromDomElement_f creator )
{
    sObjectRegistry().insert( name, creator );
}

SLink *SProjectLoader::instantiateSObjectFromDomElement( 
    const QString &name, QDomElement &element, SObject *parent )
{
    instantiateFromDomElement_f creator = sObjectRegistry().value( name );
    if( !creator ) {
        qWarning() << QString( "Internal error: Unable to instantiate object of type "+name+" !" );
        return NULL;
    }
    return creator( *this, element, parent );
}

int SProjectLoader::createObjects( SProject &project )
{
    if( !loaded_ ) {
        return 0;
    }
    // SObject *rootObject = NULL;
    QString rootId;

//    qWarning( "Internal document representation:\n%s",
//              (const char *) dom_.toString() );

    // This is kind of brute force loading, as we loop through the tree, instantiate everything
    // we can, removing it from the tree until the tree is empty.
    while(true) {
        // print out the element names of all elements that are a direct child
        // of the outermost element.
        QDomElement docElem = dom_.documentElement();
        // Read out project properties.
        {
            project.readPreChildrenAttributes( docElem );
            // TODO: Move the stuff below to postchildrenattr
             rootId = docElem.attribute( "rootId" );
             qWarning() << QString("docElem.nodeName() == ") << docElem.nodeName() << Qt::endl;
             qWarning() << "docElem.attribute rootId == " << docElem.attribute( "rootId" ) << Qt::endl;
             // TODO: Check for rootId's existence here.
        }
        // Now iterate, until all elements have been resolved.    
        QDomNode n = docElem.firstChild();
        if( n.isNull() ) {
            // All elements processed; we should be done.
            break;
        }
        do {
            QDomElement e = n.toElement(); // try to convert the node to an element.
            
            if( e.isNull() ) {
                // No element. Proceed.
                n = n.nextSibling();
                continue;
            }
            QString tagName = e.tagName();
            
            bool allChildrenKnown = true;
            
            // Now, try to instantiate. Does it have any linked children?
            QDomNode childNode = e.firstChild();
            while( !childNode.isNull() ) {
                if( childNode.isElement() ) {
                    if( childNode.nodeName() == "SLink" ) {
                        QDomElement childElement = childNode.toElement();
                        QString objectId = childElement.attribute( "objectId" );
                        // Look up the object id.
                        SLink *requestedLink = objectDict_.value( objectId );
                        if( !requestedLink ) {
                            allChildrenKnown = false;
                            break;
                        }
                    }
                }
                childNode = childNode.nextSibling();
            }
            
            if( allChildrenKnown ) {
                // All child nodes are known?
                
                // Then we can instantiate it and add it to the table.
                // Read the id.
                QString id = e.attribute( "id" );
                if( id.isNull() ) {
                    qWarning( "File is corrupt, sproject child has no \"id\"." );
                    // FIXME: Delete all SObjects.
                    return -1;
                }
                SLink *object;
                object = objectDict_.value( id );
                if( object ) {
                    qWarning() << QString("Object of type \"%1\", id \"%2\", already had been instantiated.\n").arg(tagName).arg(id);
                } else {
                    object = instantiateSObjectFromDomElement(
                        tagName, e, NULL );
                    if( !object ) {
                        qWarning() << QString( "Failed to instantiate object of type "
                                               "\"%1\" (id \"%2\"); aborting load." )
                                          .arg( tagName ).arg( id );
                        // FIXME: leaks objects already built this pass.
                        return -1;
                    }
                    // Now apply the attributes
                    object->readAttributes( e );
                    objectDict_.insert( id, object );
                } 
                QDomNode nodeToDelete = n;
                n = n.nextSibling();
                docElem.removeChild( nodeToDelete );
            } else { // Not all children known.
                n = n.nextSibling();
            } // All Children known
        } while( !n.isNull() );
    }

    // NOTE: readPostChildrenAttributes is already called in instantiateFromDomElement
    // for all objects as they are created. A second pass here was causing issues
    // with objects being processed twice, potentially corrupting state.
    // If certain objects aren't getting readPostChildrenAttributes called,
    // the fix should be to ensure they're included in the main instantiation loop.

    {
        SLink *rootLink = objectDict_.value( rootId );
        if( !rootLink ) {
            qWarning( "Root component of project was not found.\n" );
            // TODO: Allow user to select display root id.
        } else {
            project_.setRootComponent( &rootLink->getSObject() );
        }
    }
    return 0;
}

SProjectLoader::SProjectLoader( SProject &project, const QString &name )
    : project_( project ), 
      name_( name ),
      dom_( name ),
      loaded_( false )
{
    QFile f( name_ );
    if ( !f.open( QIODevice::ReadOnly ) )
        return;
    if ( !dom_.setContent( &f ) ) {
        f.close();
        return;
    }
    f.close();
    loaded_ = true;
    // objectDict_.setAutoDelete( true );
}

SProjectLoader::~SProjectLoader()
{
    // The dictionary holds the temporary "handle" SLinks returned by each
    // instantiate function (parent==NULL, not owned by anyone). The real
    // parent/child links (insertTrack, SCut ctor, setRootComponent) keep the
    // objects alive; these handles are loading scaffolding. Delete them so they
    // stop holding extra references — otherwise every loaded object keeps a
    // phantom reference forever and can never be torn down cleanly.
    for( SLink *lk : objectDict_ ) {
        delete lk;
    }
    objectDict_.clear();

}

