
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
        // Did this pass consume ANY element? Every element is either
        // instantiated or skipped, and both remove it from the document, so a
        // pass that consumes nothing can only repeat itself forever: the
        // elements that are left reference objects the file does not contain,
        // and no amount of rescanning will conjure them up. Without this the
        // loop below spins at ~3M passes/minute writing a warning per pass —
        // observed on a real project as an unbounded log and a hung load.
        bool progress = false;
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
                    // SKIP the element; do NOT abort the project.
                    //
                    // References are resolved by id (<SLink objectId='...'>), so
                    // an element without one cannot be the target of any link:
                    // nothing else in the file can reach it, and dropping it
                    // costs exactly this one object. Aborting cost the user
                    // their whole project.
                    //
                    // This is not hypothetical. Every build before proposal 08
                    // M4 wrote <SPluginSlot> without calling
                    // SObject::serializeSelfAttributes(), so every project ever
                    // saved with a plugin on a track carries an id-less slot and
                    // was permanently unopenable — by the build that wrote it as
                    // much as by any later one.
                    //
                    // The node must be REMOVED from the DOM, like the success
                    // path below: the outer while(true) rescans until the
                    // document has no children left, so an element that is
                    // neither instantiated nor removed spins forever.
                    const QString uid = e.attribute( "uid" );
                    qWarning() << QString( "Project child of type \"%1\"%2 has no "
                                           "\"id\" and was SKIPPED (written by a "
                                           "build that did not serialize it "
                                           "completely). The rest of the project "
                                           "loads; re-add that object by hand." )
                                      .arg( tagName,
                                            uid.isEmpty()
                                                ? QString()
                                                : QString( " (uid \"%1\")" ).arg( uid ) );
                    QDomNode nodeToDelete = n;
                    n = n.nextSibling();
                    docElem.removeChild( nodeToDelete );
                    progress = true;
                    continue;
                }
                SLink *object;
                object = objectDict_.value( id );
                if( object ) {
                    qWarning() << QString("Object of type \"%1\", id \"%2\", already had been instantiated.\n").arg(tagName).arg(id);
                } else {
                    object = instantiateSObjectFromDomElement(
                        tagName, e, NULL );
                    if( !object ) {
                        // SKIP the element; do NOT abort the project — same
                        // policy as the id-less branch above. The commonest
                        // cause is an <SPlainWave> whose sample file is missing
                        // (after basename recovery next to the project has
                        // already been tried, see splainwave.cpp). Dropping this
                        // one element leaves the id unresolved, so the no-progress
                        // leftover sweep below cascades the drop to any SLink/SCut
                        // that referenced it — the rest of the project loads
                        // instead of being lost over one missing file.
                        qWarning() << QString( "Failed to instantiate object of type "
                                               "\"%1\" (id \"%2\"); SKIPPED so the rest "
                                               "of the project can load." )
                                          .arg( tagName ).arg( id );
                        QDomNode nodeToDelete = n;
                        n = n.nextSibling();
                        docElem.removeChild( nodeToDelete );
                        progress = true;
                        continue;
                    }
                    // Now apply the attributes
                    object->readAttributes( e );
                    objectDict_.insert( id, object );
                }
                QDomNode nodeToDelete = n;
                n = n.nextSibling();
                docElem.removeChild( nodeToDelete );
                progress = true;
            } else { // Not all children known.
                n = n.nextSibling();
            } // All Children known
        } while( !n.isNull() );

        if( !progress ) {
            // Nothing left is resolvable. Name what dangles — the id in the
            // warning is the object the file expected to exist — then drop the
            // survivors so the rest of the project (already instantiated above)
            // survives. Dropping beats both alternatives: spinning forever, and
            // discarding a whole project over one lost reference.
            QDomNode leftover = docElem.firstChild();
            while( !leftover.isNull() ) {
                QDomElement le = leftover.toElement();
                QDomNode next = leftover.nextSibling();
                if( !le.isNull() ) {
                    QStringList missing;
                    for( QDomNode c = le.firstChild(); !c.isNull();
                         c = c.nextSibling() ) {
                        if( c.isElement() && c.nodeName() == "SLink" ) {
                            const QString oid =
                                c.toElement().attribute( "objectId" );
                            if( !objectDict_.value( oid ) ) missing << oid;
                        }
                    }
                    qWarning() << QString( "Project child of type \"%1\" (id \"%2\") "
                                           "references object(s) [%3] that the file "
                                           "does not contain; DROPPED so the rest of "
                                           "the project can load." )
                                      .arg( le.tagName(),
                                            le.attribute( "id" ),
                                            missing.join( ", " ) );
                }
                docElem.removeChild( leftover );
                leftover = next;
            }
            break;
        }
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

    // LAST: attribute-carried references (proposal 08 M4). See deferResolve().
    // After setRootComponent, so a resolver that stales the render path walks a
    // fully wired tree; before ~SProjectLoader, so the dictionary and its handle
    // links are still alive.
    runDeferredResolvers();

    return 0;
}

void SProjectLoader::deferResolve( std::function<void()> resolver )
{
    if( resolver ) deferredResolvers_.append( std::move( resolver ) );
}

void SProjectLoader::runDeferredResolvers()
{
    // Taken by value and cleared first: a resolver is allowed to register
    // another one (it would simply not run in this pass), and must never be able
    // to invalidate the list being iterated.
    QList<std::function<void()> > pending;
    pending.swap( deferredResolvers_ );
    for( const std::function<void()> &fn : pending ) {
        if( fn ) fn();
    }
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

