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
class SProjectLoader;

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
 *
 * Since proposal 08 M4 a slot round-trips through the project file: see
 * serializeSelfAttributes() for the schema and instantiateFromDomElement() for
 * the load side. A slot whose plugin is not installed keeps its descriptor and
 * its state chunk VERBATIM and re-serializes them unchanged.
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
    // Overridden to emit the <state encoding='base64'> child element (the base
    // class only writes attributes and SLink children).
    virtual int serialize( QTextStream &o );

    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader,
                                             QDomElement &element,
                                             SObject *parent );

    // Resolve a module path the way both the load path and insert-plugin do:
    // absolute as given, relative against the project's sample base dir (the
    // .qxa/.qxp directory) and then the application directory. An unresolvable
    // path is returned unchanged so the registry logs the real failure.
    static QString resolveModulePath( SProject *project, const QString &path );

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

    // Re-resolve (format, uid) against the registry and re-instantiate, then
    // re-apply the stored state chunk (proposal 08 M4). This is what a rescan
    // that finally found the plugin needs: the DSP graph is untouched (the taps
    // keep the same processor), only what process() computes changes. Returns
    // true when the slot is Active afterwards. Safe to call on an Active slot —
    // it then just rebuilds from the same descriptor. UI thread only (it
    // reaches twPlugin::prepare()); M5 owns the per-slot "Reload" affordance.
    bool reloadPlugin();

    // Bypass control
    void setBypass( bool bypass );
    bool getBypass() const { return bypass_; }

    // State persistence. saveState() reads the LIVE plugin only when the slot is
    // Active; for a Missing/Unsupported slot it hands back the stored blob
    // verbatim, which is what keeps a user's settings across opening the project
    // on a machine where the plugin is not installed.
    void saveState( std::vector<std::uint8_t> &state );
    void restoreState( const std::vector<std::uint8_t> &state );
    const std::vector<std::uint8_t> &getSavedState() const { return savedState_; }

    // A parameter (or anything else that changes what process() produces) was
    // edited from outside. Stales every cached page of this slot AND the render
    // path above it, without which the edit is inaudible.
    void notifyPluginEdited();

signals:
    void bypassChanged( bool );
    // The slot was re-instantiated (reloadPlugin()): its state/mode may have
    // changed and any editor showing its parameters is stale. M5 listens.
    void pluginReloaded();

private:
    // Materialize the processor (once) and grow the tap vector to nBuses.
    void ensureBuses( int nBuses ) const;
    // (Re-)resolve effective_ from the registry; falls back to descriptor_ with
    // its module path resolved.
    void resolveEffective();
    audio::twPluginSlotProcessor::Factory makeFactory() const;

    audio::twPluginDescriptor descriptor_;   // as stored / as given (VERBATIM)
    audio::twPluginDescriptor effective_;    // as resolved against the registry

    std::shared_ptr<audio::twPluginSlotProcessor>          proc_;
    std::vector<std::shared_ptr<audio::twPluginInsert> >   taps_;   // one per bus
    int  busCount_ = 0;
    bool bypass_ = false;
    std::vector<std::uint8_t> savedState_;  // opaque plugin state chunk
};

#endif
