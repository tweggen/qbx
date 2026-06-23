#ifndef _SPLUGINCHAIN_H_
#define _SPLUGINCHAIN_H_

#include "sobject.h"

class twComponent;
class SPluginSlot;
class SProjectLoader;
class QDomElement;

/**
 * A container object that holds an ordered chain of SPluginSlot children.
 * This is a dedicated child container on an STrack, keeping inserts visually
 * and logically distinct from clip/child-track children.
 *
 * The DSP graph threads audio through each insert in order:
 * track-mixer → slot0 → slot1 → ... → slotN → rewire
 */
class SPluginChain : public SObject {
    Q_OBJECT
public:
    SPluginChain( SProject *project );
    virtual ~SPluginChain();

    // SObject interface
    virtual twComponent &getRootComponent();
    virtual QWidget *getDetailEditWidget( QWidget *parent = nullptr );
    virtual QWidget *getInlineEditWidget( QWidget *parent = nullptr );
    virtual SObjectRenderer *getInlineRenderer();

    // Serialization
    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader,
                                              QDomElement &element,
                                              SObject *parent );

    // Container access
    SPluginSlot *getSlotAt( int index ) const;
    int getSlotCount() const { return childCount(); }

    // Reorder slots
    void reorderSlot( int fromIndex, int toIndex );

signals:
    void slotInserted( int index, SPluginSlot &slot );
    void slotRemoved( int index, SPluginSlot &slot );
    void slotsReordered();

protected:
    void childEvent( QChildEvent *event ) override;

private:
    twComponent *getChainComponent();
    twComponent *chainComponent_ = nullptr;
};

#endif
