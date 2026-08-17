#ifndef _SPLUGINCHAIN_H_
#define _SPLUGINCHAIN_H_

#include "app/model/sobject.h"

#include <functional>

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

    // Where the DSP component for this chain comes from (proposal 08 M4).
    //
    // A chain owns NO component of its own: STrack builds ONE twPluginChain of
    // the project's channel width, and this model object is the ordered
    // container in front of it. So the owning track installs a provider that
    // hands out that component, and
    //
    // (This read "one twPluginChain per BUS (the frozen-page model is one mono
    // page per component)". Both halves retired at proposal 36 B4: the page
    // carries N planar channels, so N channels are no longer N component
    // instances, and the per-bus instantiation went with the plugin taps.)
    //
    // getRootComponent() answers with it instead of throwing — which is what it
    // used to do UNCONDITIONALLY, because getChainComponent() was a TODO stub
    // returning a member nothing ever assigned. An unowned chain (one loaded
    // from a file before its track adopts it) answers null, which is honest.
    using ComponentProvider = std::function<std::shared_ptr<twComponent>()>;
    void setComponentProvider( ComponentProvider provider )
    {
        componentProvider_ = std::move( provider );
    }

signals:
    void slotInserted( int index, SPluginSlot &slot );
    void slotRemoved( int index, SPluginSlot &slot );
    // Carries the indices because the DSP chain has to make the SAME move, not
    // merely re-wire what it already has: a listener that only rebuilds keeps
    // the old order. Receivers that just want to repaint may ignore both.
    void slotsReordered( int fromIndex, int toIndex );

protected:
    void childEvent( QChildEvent *event ) override;

private:
    std::shared_ptr<twComponent> getChainComponent();
    ComponentProvider componentProvider_;
};

#endif
