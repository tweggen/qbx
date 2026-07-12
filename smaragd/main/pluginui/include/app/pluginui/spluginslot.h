#ifndef _SPLUGINSLOT_H_
#define _SPLUGINSLOT_H_

#include "app/model/sobject.h"
#include "tw/plugins/twplugindescriptor.h"
#include <memory>

namespace audio {
class twPlugin;
class twPluginInsert;
}  // namespace audio

class SApplication;

/**
 * A model object wrapping one plugin insert in a track's effect chain.
 * Each slot stores the plugin descriptor, opaque state chunk, and bypass flag.
 * The backing DSP components are twPluginInsert instances (one per bus).
 */
class SPluginSlot : public SObject {
    Q_OBJECT
public:
    SPluginSlot( SProject *project, const audio::twPluginDescriptor &desc );
    virtual ~SPluginSlot();

    // SObject interface
    virtual twComponent &getRootComponent();
    virtual QWidget *getDetailEditWidget( QWidget *parent = nullptr );
    virtual QWidget *getInlineEditWidget( QWidget *parent = nullptr );
    virtual SObjectRenderer *getInlineRenderer();

    virtual int readPreChildrenAttributes( QDomElement &element );
    virtual int serializeSelfAttributes( QTextStream &o );

    // Plugin access (for a single bus - returns bus 0 for backward compatibility)
    audio::twPluginInsert *getInsert() const { return getInsertForBus(0); }
    audio::twPluginInsert *getInsertForBus( int busIndex ) const;

    const audio::twPluginDescriptor &getDescriptor() const { return descriptor_; }

    // Bypass control
    void setBypass( bool bypass );
    bool getBypass() const { return bypass_; }

    // State persistence
    void saveState( std::vector<std::uint8_t> &state );
    void restoreState( const std::vector<std::uint8_t> &state );
    void serializeStateChunk( QDomElement &parentElem, QDomDocument &doc );

signals:
    void bypassChanged( bool );

private:
    audio::twPluginDescriptor descriptor_;
    std::vector<std::unique_ptr<audio::twPluginInsert>> inserts_;  // one per bus
    bool bypass_ = false;
    std::vector<std::uint8_t> savedState_;  // opaque plugin state chunk
};

#endif
