#ifndef _SPROJECTLOADER_H_
#define _SPROJECTLOADER_H_

#include <QDomDocument>
#include <QHash>
#include <QList>
#include <functional>
#include "app/model/slink.h"

class SProject;
typedef QHash<QString,SLink*> SObjectDictionary;

/**
 * How the leftover sweep may repair an element whose <SLink objectId='…'>
 * names an object the file does not contain (proposal 37 D8a).
 *
 *  Plain     — drop the ELEMENT (the historic behaviour; it is the only safe
 *              answer for a type the loader knows nothing structural about).
 *  Container — drop the dangling LINK and keep the element: a track, a mixer
 *              or a take stack that lost one child is still itself, and every
 *              other child on it is still the user's work.
 *  Window    — drop the ELEMENT: a window (SCut, later SMidiCut) whose CONTENT
 *              is gone has nothing left to show. Its own placement then falls
 *              under the Container rule on the next pass.
 *
 * The kind is declared at registration by the slice that owns the element, so
 * the loader still names no concrete type.
 */
enum class SElementKind { Plain, Container, Window };

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
    static void registerSObjectClass( const QString &name,
                                     instantiateFromDomElement_f creationFunction,
                                     SElementKind kind = SElementKind::Plain );
    /** Registered repair policy for an element name; Plain when unknown. */
    static SElementKind elementKind( const QString &name );
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
    static QHash<QString, SElementKind> &sObjectKinds();

    /**
     * One prune pass over the elements the instantiation loop could not
     * consume (D8a). Applies the per-kind policy above and returns true if it
     * changed the document, so createObjects() can retry to a fixed point.
     */
    bool pruneUnresolvable_( QDomElement &docElem );
    SObjectDictionary objectDict_;
    QList<std::function<void()> > deferredResolvers_;
    SProject &project_;
    QString name_;
    QDomDocument dom_;
    bool loaded_;
};

#endif
