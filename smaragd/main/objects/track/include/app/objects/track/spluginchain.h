#ifndef _SPLUGINCHAIN_H_
#define _SPLUGINCHAIN_H_

#include "app/model/sobject.h"

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
    virtual std::shared_ptr<twComponent> getRootComponent() override;
    virtual QWidget *getDetailEditWidget( QWidget *parent = nullptr ) override;
    virtual QWidget *getInlineEditWidget( QWidget *parent = nullptr ) override;
    virtual SObjectRenderer *getInlineRenderer() override;

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
    std::shared_ptr<twComponent> getChainComponent();
    std::shared_ptr<twComponent> chainComponent_;
};

#endif
