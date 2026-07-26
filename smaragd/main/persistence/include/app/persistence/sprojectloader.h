#ifndef _SPROJECTLOADER_H_
#define _SPROJECTLOADER_H_

#include <QDomDocument>
#include <QHash>
#include <QList>
#include <functional>
#include "app/model/slink.h"

class SProject;
typedef QHash<QString,SLink*> SObjectDictionary;

class SProjectLoader 
{
public:
    SProjectLoader( SProject &, const QString & );
    ~SProjectLoader();

    typedef SLink *(*instantiateFromDomElement_f)(
        SProjectLoader &, QDomElement &, SObject *parent );

    bool wasLoaded() const { return loaded_; }
    
    int createObjects( SProject &project );
    SObjectDictionary &getObjectDictionary() {return objectDict_;};

    // Type registry: each object slice self-registers its element name from a
    // static initializer in its own .cpp (proposal 14, Phase 5) — the loader
    // names NO concrete types. Requires the app to stay an OBJECT library
    // (a STATIC lib would drop the registration TUs; see main/CMakeLists.txt).
    static void registerSObjectClass( const QString &name, instantiateFromDomElement_f creationFunction );
    SLink *instantiateSObjectFromDomElement(
        const QString &name, QDomElement &, SObject *parent );

    SProject &getProject() { return project_; }

    /**
     * Queue work to run once EVERY object exists (proposal 08 M4).
     *
     * The instantiation loop only guarantees resolution for references written
     * as <SLink objectId='…'> CHILDREN: it defers an element until each of those
     * ids is in the dictionary. A reference carried by a plain ATTRIBUTE —
     * STrack's pluginChainId, say — is invisible to that ordering, so an object
     * that reads one during its own construction may look it up before it has
     * been built. Registering a resolver here instead moves that lookup to the
     * end of createObjects(), when the dictionary is complete but the loader
     * (and therefore the dictionary and the temporary handle SLinks) is still
     * alive.
     *
     * Resolvers run once, in registration order, on the loading (UI) thread.
     * Anything a resolver keeps must take its own reference: ~SProjectLoader
     * deletes the handle links right afterwards.
     */
    void deferResolve( std::function<void()> resolver );

protected:
private:
    // Drains deferredResolvers_ (in order, clearing as it goes).
    void runDeferredResolvers();

    // Accessor for the process-wide type registry (function-local static
    // avoids the static-initialization-order fiasco with the registrants).
    static QHash<QString, instantiateFromDomElement_f> &sObjectRegistry();
    SObjectDictionary objectDict_;
    QList<std::function<void()> > deferredResolvers_;
    SProject &project_;
    QString name_;
    QDomDocument dom_;
    bool loaded_;
};

#endif
