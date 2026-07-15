
#ifndef _SLINK_H_
#define _SLINK_H_

#include <qobject.h>
#include "tw/graph/tw303aenv.h"
#include "app/model/sobject.h"
#include "tw/core/twfraction.h"

class QWidget;
class twComponent;
class QTextStream;

/**
 * SLink wraps an SObject and adds timing information (startTime) to place content
 * into a timeline. SLink instances represent references to resources (whether
 * loaded from files via SCut, or other SObjects) positioned at specific times
 * within a parent container (typically an STrack).
 *
 * IMPORTANT: Always construct with parent=NULL, then call setParent() after
 * construction is complete. This avoids triggering childEvent() during construction,
 * which could call virtual methods on an incompletely-initialized object.
 */
class SLink
    : public QObject 
{
    Q_OBJECT
public:
    SLink( SObject &sobject, SObject *parent=0 );
    SLink( const SLink & );
    virtual ~SLink();

    SObject &getSObject() const
        { return object_; }

    virtual int serialize( QTextStream & );
    virtual int serializeSelfAttributes( QTextStream & );
    virtual int readAttributes( QDomElement & e );

    virtual std::shared_ptr<twComponent> getRootComponent() const;
    virtual QWidget *getDetailEditWidget( QWidget *parent = NULL );
    virtual QWidget *getInlineEditWidget( QWidget *parent = NULL );
    
    virtual bool hasStartTime() const;
    virtual offset_t getStartTime() const;

    virtual int seekTo( offset_t );
    virtual bool isEmpty() const;

    /**
     * implement comparison operators regarding start time.
     * Note that we do not check if start time was defined.
     * If you use that operator, you have to check it for yourselves.
     */
    int operator < ( SLink &other ) {
        return startTime_<other.startTime_; }
    int operator == ( SLink &other )  {
        return startTime_<other.startTime_; }

signals:
    /**
     * The link was moved in time.
     */
    void startTimeChanged( offset_t newTime );

public slots:
    /**
     * Set a different start time.
     */
    void setStartTime( offset_t );

protected:
private:
    offset_t startTime_;
    SObject &object_;
};

#endif
