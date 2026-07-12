
#ifndef _SOBJECT_RENDERER_H
#define _SOBJECT_RENDERER_H

#include <QRect>
#include <QPainter>
#include <qobject.h>
#include "tw/graph/tw303aenv.h"

class SObject;
class SLink;

class SRenderContext
{
public:
    SRenderContext( QPainter & );
    virtual ~SRenderContext();

    QPainter &getPainter() const { return painter_; }
    const QRect &getVisibRect() const { return visibRect_; }

    void setVisibRect( const QRect & );

    /**
     * Return the absolute time position of the given visible x coordinate.
     * This of course implies, that x is the time dimension.
     */
    virtual offset_t getTimeOf( int x ) const = 0;

protected:
private:
    QPainter &painter_;
    QRect visibRect_;
    offset_t nsPerPixel_;
};


class SObjectRenderer 
    : public QObject
{
    Q_OBJECT
public:
    SObjectRenderer( SObject & );
    ~SObjectRenderer();

    virtual void draw( SLink &, SRenderContext & ) = 0;
    SObject &getObject() const { return sobject_; }
    
protected:
    SObject &sobject_;
private:
};

#endif

