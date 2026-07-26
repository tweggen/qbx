#ifndef _SPLUGINSLOT_H_
#define _SPLUGINSLOT_H_

#include "app/model/sobject.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twpluginslotproc.h"
#include <memory>
#include <vector>

namespace audio {
class twPlugin;
class twPluginInsert;
class twPluginSlotProcessor;
}  // namespace audio

class SApplication;

/**
 * A model object wrapping one plugin insert in a track's effect chain.
 * Each slot stores the plugin descriptor, opaque state chunk, and bypass flag.
 *
 * Since proposal 08 M3 the DSP side of a slot is ONE shared
 * audio::twPluginSlotProcessor (it owns the twPlugin instance(s), the block
 * chunking and the channel-mismatch mapping) plus one audio::twPluginInsert
 * TAP per bus. The taps are 1-in/1-out components, which is what the engine's
 * one-mono-page-per-component model requires; channel coherence lives in the
 * processor, not in the components.
 */
class SPluginSlot : public SObject {
    Q_OBJECT
public:
    SPluginSlot( SProject *project, const audio::twPluginDescriptor &desc );
    virtual ~SPluginSlot();

    // SObject interface
    virtual std::shared_ptr<twComponent> getRootComponent();
    virtual QWidget *getDetailEditWidget( QWidget *parent = nullptr );
    virtual QWidget *getInlineEditWidget( QWidget *parent = nullptr );
    virtual SObjectRenderer *getInlineRenderer();

    virtual int readPreChildrenAttributes( QDomElement &element );
    virtual int serializeSelfAttributes( QTextStream &o );

    // Plugin access (bus 0 for backward compatibility)
    std::shared_ptr<audio::twPluginInsert> getInsert() const { return getInsertForBus(0); }
    std::shared_ptr<audio::twPluginInsert> getInsertForBus( int busIndex ) const;

    // Declare how many parallel mono buses this slot serves. Called by STrack
    // before the taps are wired; re-deriving the channel-mismatch mapping.
    void setBusCount( int nBuses );

    const audio::twPluginDescriptor &getDescriptor() const { return descriptor_; }

    // The descriptor actually used to instantiate: the registry's own record for
    // (format, uid) when the scan knows it, otherwise the stored one.
    const audio::twPluginDescriptor &getEffectiveDescriptor() const { return effective_; }

    audio::twPluginSlotState getSlotState() const;
    audio::twPluginSlotMode  getSlotMode() const;

    const std::shared_ptr<audio::twPluginSlotProcessor> &getProcessor() const { return proc_; }

    // Bypass control
    void setBypass( bool bypass );
    bool getBypass() const { return bypass_; }

    // State persistence
    void saveState( std::vector<std::uint8_t> &state );
    void restoreState( const std::vector<std::uint8_t> &state );
    void serializeStateChunk( QDomElement &parentElem, QDomDocument &doc );

    // A parameter (or anything else that changes what process() produces) was
    // edited from outside. Stales every cached page of this slot AND the render
    // path above it, without which the edit is inaudible.
    void notifyPluginEdited();

signals:
    void bypassChanged( bool );

private:
    // Materialize the processor (once) and grow the tap vector to nBuses.
    void ensureBuses( int nBuses ) const;

    audio::twPluginDescriptor descriptor_;   // as stored / as given
    audio::twPluginDescriptor effective_;    // as resolved against the registry

    std::shared_ptr<audio::twPluginSlotProcessor>          proc_;
    std::vector<std::shared_ptr<audio::twPluginInsert> >   taps_;   // one per bus
    int  busCount_ = 0;
    bool bypass_ = false;
    std::vector<std::uint8_t> savedState_;  // opaque plugin state chunk
};

#endif
