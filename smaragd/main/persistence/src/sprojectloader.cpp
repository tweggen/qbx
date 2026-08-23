
#include <stdlib.h>

#include <qfile.h>
#include <QDebug>
#include <QSet>

#include <iostream>

using namespace std;

#include "app/persistence/sprojectloader.h"
#include <QVector>

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

QHash<QString, SElementKind> &SProjectLoader::sObjectKinds()
{
    static QHash<QString, SElementKind> kinds;
    return kinds;
}

namespace {
// File-static, like the class registry below: a plain vector, appended by
// static initializers before main() and read once per load.
QVector<SProjectLoader::postLoadPass_f> &postLoadPasses()
{
    static QVector<SProjectLoader::postLoadPass_f> v;
    return v;
}
}   // namespace

void SProjectLoader::registerPostLoadPass( postLoadPass_f fn )
{
    if( fn ) postLoadPasses().append( fn );
}

void SProjectLoader::runPostLoadPasses( SProject &project )
{
    for( postLoadPass_f fn : postLoadPasses() ) fn( project );
}

void SProjectLoader::registerSObjectClass(
    const QString &name, SProjectLoader::instantiateFromDomElement_f creator,
    SElementKind kind )
{
    sObjectRegistry().insert( name, creator );
    sObjectKinds().insert( name, kind );
}

SElementKind SProjectLoader::elementKind( const QString &name )
{
    return sObjectKinds().value( name, SElementKind::Plain );
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
            // Nothing left is resolvable by rescanning. PRUNE, then RETRY
            // (proposal 37 D8a): the repair is per element KIND, and each
            // repair can unblock elements the next pass CAN consume — a
            // container that loses one dangling link becomes instantiable, and
            // dropping a dead window turns its own placement into a dangling
            // link that the container rule then handles. Iterate to a fixed
            // point.
            //
            // The historic behaviour (drop every leftover at once) is what a
            // Plain element still gets, and is the backstop when a pass
            // repairs nothing at all — the loop must always terminate.
            if( pruneUnresolvable_( docElem ) ) {
                continue;
            }
            QDomNode leftover = docElem.firstChild();
            while( !leftover.isNull() ) {
                QDomNode next = leftover.nextSibling();
                const QDomElement le = leftover.toElement();
                if( !le.isNull() ) {
                    // Reachable only for a set of elements that reference each
                    // other and nothing outside itself (a cycle the file
                    // describes but the loader cannot enter). Name it: every
                    // recovery warns, and a silent drop would be the one thing
                    // worse than the abort all of this replaces.
                    qWarning() << QString( "Project child of type \"%1\" (id \"%2\") "
                                           "cannot be resolved in any order; "
                                           "DROPPED so the rest of the project "
                                           "can load." )
                                      .arg( le.tagName(), le.attribute( "id" ) );
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
            // The one loss there is no recovery from: everything the document
            // describes hangs off the root, so a project without one is not a
            // damaged project, it is no project. FAIL the load (proposal 37
            // D8a) instead of handing the caller an empty shell that looks
            // like a successful open and would overwrite the file on save.
            qWarning() << QString( "Root component of project (rootId \"%1\") was "
                                   "not found; the project cannot be loaded." )
                              .arg( rootId );
            return -1;
        }
        project_.setRootComponent( &rootLink->getSObject() );
    }

    // LAST: attribute-carried references (proposal 08 M4). See deferResolve().
    // After setRootComponent, so a resolver that stales the render path walks a
    // fully wired tree; before ~SProjectLoader, so the dictionary and its handle
    // links are still alive.
    runDeferredResolvers();

    return 0;
}

// One prune pass over what the instantiation loop could not consume. Every
// leftover element has at least one <SLink objectId='…'> naming an object the
// document does not contain (an element whose link children all resolved would
// have been instantiated), so the policy below always has something to name.
//
// Returns true if the document changed — createObjects() then RETRIES the
// instantiation loop, because a repair here routinely makes another element
// resolvable.
bool SProjectLoader::pruneUnresolvable_( QDomElement &docElem )
{
    // A link is only UNRESOLVABLE if its target is neither built yet nor still
    // in the document. Without the second half this pass would cascade exactly
    // as the old sweep did, one level per call: at the moment it runs, a track
    // that is waiting for its clip is itself missing from the dictionary, so
    // the mixer's link to that track looks dangling too — and dropping it
    // would lose the whole track while the repair that was about to save it
    // (dropping ONE link) had not been retried yet.
    QSet<QString> stillInDocument;
    for( QDomNode c = docElem.firstChild(); !c.isNull(); c = c.nextSibling() ) {
        if( !c.isElement() ) continue;
        const QString id = c.toElement().attribute( "id" );
        if( !id.isEmpty() ) stillInDocument.insert( id );
    }

    bool changed = false;
    QDomNode leftover = docElem.firstChild();
    while( !leftover.isNull() ) {
        QDomElement le = leftover.toElement();
        QDomNode next = leftover.nextSibling();
        if( le.isNull() ) {
            leftover = next;
            continue;
        }

        // This element's dangling link children.
        QList<QDomNode> dangling;
        QStringList missing;
        for( QDomNode c = le.firstChild(); !c.isNull(); c = c.nextSibling() ) {
            if( c.isElement() && c.nodeName() == "SLink" ) {
                const QString oid = c.toElement().attribute( "objectId" );
                if( !objectDict_.value( oid )
                    && !stillInDocument.contains( oid ) ) {
                    dangling.append( c );
                    missing << oid;
                }
            }
        }
        if( dangling.isEmpty() ) {
            leftover = next;
            continue;
        }

        const SElementKind kind = elementKind( le.tagName() );
        if( kind == SElementKind::Container ) {
            // Drop the LINK, keep the container: a track that lost one clip is
            // still the user's track. One warning per lost child — a silent
            // recovery would be worse than the abort it replaces.
            for( QDomNode &d : dangling ) {
                qWarning() << QString( "Project child of type \"%1\" (id \"%2\") "
                                       "links object \"%3\", which the file does "
                                       "not contain; the LINK was dropped and the "
                                       "rest of the container loads." )
                                  .arg( le.tagName(), le.attribute( "id" ),
                                        d.toElement().attribute( "objectId" ) );
                le.removeChild( d );
            }
            changed = true;
        } else {
            // Window: its content is gone, so there is nothing left to window
            // and it goes. Plain: the historic drop. Either way the element
            // leaves, and any container that linked it meets the rule above on
            // the next pass.
            qWarning() << QString( "Project child of type \"%1\" (id \"%2\") "
                                   "references object(s) [%3] that the file does "
                                   "not contain; %4 DROPPED so the rest of the "
                                   "project can load." )
                              .arg( le.tagName(), le.attribute( "id" ),
                                    missing.join( ", " ),
                                    kind == SElementKind::Window
                                        ? QString( "the window object was" )
                                        : QString( "it was" ) );
            docElem.removeChild( leftover );
            changed = true;
        }
        leftover = next;
    }
    return changed;
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

