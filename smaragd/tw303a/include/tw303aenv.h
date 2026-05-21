
#ifndef _TW303AENV_
#define _TW303AENV_

#include <qobject.h>
#include <qlist.h>

#include "twcomponent.h"

QString QuoteString( const QString & );
QString UnquoteString( const QString & );


class twComponent;

class tw303aEnvironment 
    : QObject
{
    Q_OBJECT
private:
    QList<twComponent*> listModules;
    
    length_t bufferSize;
    int sampleRate;
protected:
public:
    tw303aEnvironment();
    virtual ~tw303aEnvironment();
    
signals:
    void componentAdded( twComponent & );
    void componentRemoved( twComponent & );

public:
    void setBufferSize( length_t size ) { bufferSize=size; };
    length_t getBufferSize() const { return bufferSize; };
    void addModule( twComponent *mod );
    void removeModule( twComponent *mod );
    int getSRate() const { return sampleRate; }
};

#endif
