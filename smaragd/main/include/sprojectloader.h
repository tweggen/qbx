#ifndef _SPROJECTLOADER_H_
#define _SPROJECTLOADER_H_

#include <qxml.h>
#include <qdom.h>
#include <qhash.h>
#include "slink.h"

class SProject;
typedef QHash<QString,SLink*> SObjectDictionary;

struct SObjectRegistryEntry;

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

    void registerSObjectClass( const QString &nane, instantiateFromDomElement_f creationFunction );
    SLink *instantiateSObjectFromDomElement(
        const QString &name, QDomElement &, SObject *parent );

    SProject &getProject() { return project_; }

protected:
private:
    QHash<QString,SObjectRegistryEntry*> sObjectRegistry_;
    SObjectDictionary objectDict_;
    SProject &project_;
    QString name_;
    QDomDocument dom_;
    bool loaded_;
};

struct SObjectRegistryEntry {
    SProjectLoader::instantiateFromDomElement_f creator_;
    QString name_;
};


#endif
