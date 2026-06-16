#ifndef _SPLUGINSLOT_H_
#define _SPLUGINSLOT_H_

#include "sobject.h"
#include "plugins/twplugindescriptor.h"
#include <memory>

namespace audio {
class twPlugin;
class twPluginInsert;
}  // namespace audio

class SApplication;

/**
 * A model object wrapping one plugin insert in a track's effect chain.
 * Each slot stores the plugin descriptor, opaque state chunk, and bypass flag.
 * The backing DSP component is a twPluginInsert.
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

    // Plugin access
    audio::twPluginInsert *getInsert() const { return insert_.get(); }
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
    std::unique_ptr<audio::twPluginInsert> insert_;
    bool bypass_ = false;
    std::vector<std::uint8_t> savedState_;  // opaque plugin state chunk
};

#endif
