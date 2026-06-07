
#ifndef _SOBJECT_H
#define _SOBJECT_H

#include <qobject.h>
#include "tw303aenv.h"
#include <qtextstream.h>
#include <QDomElement>
#include <QList>

class QWidget;

class twComponent;
class SProject;
class SLink;
class SObjectRenderer;


/**
 * A lightweight, storage-agnostic view over a container's ordered SLink
 * children. Consumers iterate with a range-for
 * (`for( SLink *lk : obj->childLinks() )`) or index with at()/size(), and stay
 * decoupled from how the order is actually stored (an explicit list today, the
 * QObject child list historically). Order is owned by the container; QObject
 * parentage is only about lifetime.
 */
class SChildLinks {
public:
    using const_iterator = QList<SLink*>::const_iterator;
    explicit SChildLinks( const QList<SLink*> &list ) : list_( list ) {}
    const_iterator begin() const { return list_.cbegin(); }
    const_iterator end() const { return list_.cend(); }
    int size() const { return list_.size(); }
    bool isEmpty() const { return list_.isEmpty(); }
    SLink *at( int i ) const { return list_.at( i ); }
private:
    const QList<SLink*> &list_;
};


/**
 * This is QBX generic data container.
 * All data containers are children of the project object.
 * They linked together by SLink objects.
 *
 * All things marked as properties are user editable.
 */
class SObject 
    : public QObject
{
    Q_OBJECT
    Q_PROPERTY( bool Solo READ isSolo WRITE setSolo )
    Q_PROPERTY( bool Muted READ isMuted WRITE setMuted )
    Q_PROPERTY( double Volume READ getVolume WRITE setVolume )
    Q_PROPERTY( double Pan READ getPan WRITE setPan )
    Q_PROPERTY( double Delay READ getDelay WRITE setDelay )
    Q_PROPERTY( QString SName READ getSName WRITE setSName )
        
//Q_PROPERTY( type name READ getFunction [WRITE setFunction]
//            [STORED bool] [DESIGNABLE bool] [RESET resetFunction])


public:
    SObject( SProject* );
    virtual ~SObject();

    SProject &getProject() const { return *(SProject *)parent(); }

    virtual int serialize( QTextStream & );
    virtual int readPreChildrenAttributes( QDomElement &element );
    virtual int readPostChildrenAttributes( QDomElement &element );

    /**
     * Return the DSP root component from the SObject.
     * Its outputs may be connected in various ways, as the SObject
     * may be included at different parts of the entire arrangement.
     */
    virtual twComponent &getRootComponent() = 0;
    /**
     * Return a widget suitable for full-screen editing this SObject.
     */
    virtual QWidget *getDetailEditWidget( QWidget *parent=NULL ) = 0;
    /**
     * Don't know what, but provided...
     */
    virtual QWidget *getInlineEditWidget( QWidget *parent=NULL ) = 0;
    
    virtual SObjectRenderer *getInlineRenderer() = 0;        

    /**
     * Seek the underlying twComponents to a given point, so that subsequent
     * playback will start there.
     * This method is provided inside the objects to facilate different underlying 
     * interfaces.
     */
    virtual int seekTo( offset_t );

    /**
     * Ordered view of this container's SLink children. Prefer this and the
     * childAt()/childCount() accessors over QObject::children() everywhere order
     * matters, so call sites stay decoupled from the storage.
     */
    SChildLinks childLinks() const { return SChildLinks( childOrder_ ); }
    int childCount() const { return childOrder_.size(); }
    SLink *childAt( int index ) const;
    int indexOfChild( const SLink *child ) const;
    int indexOfChildObject( const SObject &child ) const;

    /**
     * Reorder this container's SLink children so the child currently at
     * fromIndex ends up at toIndex (the others shift to fill). No-op if either
     * index is out of range or they are equal. Order is just the explicit list,
     * so this is a plain list move — QObject parentage and refcounts are
     * untouched and no childObject signals fire.
     */
    void moveChildToIndex( int fromIndex, int toIndex );

    /**
     * Return the number of references to this object (from SLinks)
     */
    int getNReferences() const;
    
    /**
     * Returns true, if this object has no links to other objects.
     */
    virtual bool isEmpty() const;

    virtual bool hasDuration() const;
    virtual length_t getDuration() const;    
    
    virtual bool hasPreview() const;
    virtual int getPreview( preview_t *dest, 
                    offset_t start, length_t length, 
                    offset_t nProbes );

    // User properties.
    bool isSolo() const
        { return solo_; }
    bool isMuted() const
        { return muted_; }
    double getVolume() const
        { return volume_; }
    double getPan() const
        { return pan_; }
    double getDelay() const
        { return delay_; }
    QString getSName() const
        { return sName_; }    

public slots:    
    void setSolo( bool );
    void setMuted( bool );
    void setVolume( double );
    void setPan( double );
    void setDelay( double );
    void setSName( const QString & );    

signals:

signals:

    // For the properties
    void soloChanged( bool );
    void mutedChanged( bool );
    void volumeChanged( double );
    void panChanged( double );
    void delayChanged( double );
    void sNameChanged( const QString & );


    /**
     * Ths link's duration changed.
     */
    void durationChanged( length_t newDuration );

    /**
     * Child object was added.
     */
    void childObjectAdded( SLink &child );
    
    /**
     * Child object was removed.
     * At calling time, the link is not yet deleted.
     */ 
    void childObjectRemoved( SLink &child );

    /**
     * This signal is emitted, if the object becoms unreferenced.
     */
    void gotUnreferenced();

    /**
     * This signal is emitted, if the object receives its first reference.
     */
    void gotReferenced();

    void nRefsChanged();

public slots:
    /**
     * Set a different duration.
     */
    virtual void setDuration( length_t );

    /**
     * Add a reference.
     */
    void addRef();

    /**
     * Remove a reference.
     */
    void removeRef();

    /**
     * Forget the current preview.
     */
    virtual void invalidatePreview();

protected:
    offset_t getChildrenExtent( offset_t &firstStart, offset_t &lastEnd, 
                                int &nUndefStart, int &nUndefDuration ) const;
    offset_t getFirstChildStartTime() const;
    length_t getAllChildsDuration() const;

    int getStraightPreview( preview_t *, offset_t, length_t, offset_t );
    virtual void childEvent( QChildEvent * );

    virtual int serializeSelfAttributes( QTextStream &o );

    int getChildIndex( SObject & ) const;

private:
    void gotChild( SLink & );
    void lostChild( SLink & );
    int straightCalcPreviewData();
    // Source of truth for child order (membership mirrors QObject::children(),
    // maintained in childEvent(); order is independent and set by
    // moveChildToIndex()).
    QList<SLink*> childOrder_;
    int nRefs_;
    offset_t previewForLength_;
    offset_t nPreviewProbes_;
    preview_t *previewData_;
    offset_t previewSkip_;

    bool solo_;
    bool muted_;
    double volume_;
    double pan_;
    double delay_;
    QString sName_;
};

#endif
