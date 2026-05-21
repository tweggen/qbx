
#include <stdlib.h>

#include <qfile.h>
#include <QDebug>

#include <iostream>

using namespace std;

#include "sapplication.h"
#include "sprojectloader.h"

#include "sobject.h"
#include "sproject.h"
#include "slink.h"

// For the factory stuff.
#include "splainwave.h"
#include "strack.h"
#include "scut.h"
#include "sstdmixer.h"
//#include "sgrainfile.h"


void SProjectLoader::registerSObjectClass( 
    const QString &name, SProjectLoader::instantiateFromDomElement_f creator )
{
    SObjectRegistryEntry *entry = new SObjectRegistryEntry;
    entry->name_ = name;
    entry->creator_ = creator;
    sObjectRegistry_.insert( name, entry );
}

SLink *SProjectLoader::instantiateSObjectFromDomElement( 
    const QString &name, QDomElement &element, SObject *parent )
{
    SObjectRegistryEntry *entry = sObjectRegistry_.value( name );    
    if( !entry ) {
        qWarning() << QString( "Internal error: Unable to instantiate object of type "+name+" !" );
        return NULL;
    }
    return entry->creator_( *this, element, parent );
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
             qWarning() << QString("docElem.nodeName() == ") << docElem.nodeName() << endl;
             qWarning() << "docElem.attribute rootId == " << docElem.attribute( "rootId" ) << endl;
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
                    qWarning() << "childNode.nodeName() == " << childNode.nodeName() << endl;
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
                if( id==QString::null ) {
                    qWarning( "File is corrupt, sproject child has no \"id\"." );
                    // FIXME: Delete all SObjects.
                    return -1;
                }
                SLink *object;
                object = objectDict_.value( id );
                if( object ) {
                    qWarning() << QString("Object of type \"%1\", id \"%2\", already had been instantiated.\n").arg(tagName).arg(id);
                } else {
                    qWarning() << QString( "Instantiating object of type \"%1\" with id=\"%2\".\n" )
                        .arg( tagName ).arg( id );
                    object = instantiateSObjectFromDomElement(
                        tagName, e, NULL );
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
    registerSObjectClass( "SPlainWave", SPlainWave::instantiateFromDomElement );
    registerSObjectClass( "SStdMixer", SStdMixer::instantiateFromDomElement );
    registerSObjectClass( "STrack", STrack::instantiateFromDomElement );
    registerSObjectClass( "SCut", SCut::instantiateFromDomElement );

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
    for( auto it = sObjectRegistry_.begin(); it != sObjectRegistry_.end(); ++it ) {
        sObjectRegistry_.erase(it); 
    }
}

