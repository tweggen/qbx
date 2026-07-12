#ifndef _SPROJECTLOADER_H_
#define _SPROJECTLOADER_H_

#include <QDomDocument>
#include <QHash>
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

protected:
private:
    // Accessor for the process-wide type registry (function-local static
    // avoids the static-initialization-order fiasco with the registrants).
    static QHash<QString, instantiateFromDomElement_f> &sObjectRegistry();
    SObjectDictionary objectDict_;
    SProject &project_;
    QString name_;
    QDomDocument dom_;
    bool loaded_;
};

#endif
