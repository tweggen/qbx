#ifndef _SPLUGINSLOT_H_
#define _SPLUGINSLOT_H_

#include "app/model/sobject.h"
#include "tw/events/tweventsource.h"
#include "tw/events/twtempomap.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twpluginslotproc.h"
#include <cstdint>
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

    // The slot's ONE insert (proposal 36 B4 retired the per-bus taps: a slot is
    // a single component N channels wide). Materializes it on demand.
    std::shared_ptr<audio::twPluginInsert> getInsert() const;

    // Like getInsert(), but NEVER materializes: null if the insert does not
    // already exist. For teardown paths (STrack::onPluginSlotRemoved), which
    // need the identity of what is in the chain and must not instantiate a
    // plugin on the way out.
    std::shared_ptr<audio::twPluginInsert> peekInsert() const;

    // Declare how many CHANNELS this slot processes — the width of the pages
    // its insert is handed. Called by STrack before the insert is wired,
    // because the count is what selects the channel-mismatch mapping. Shrinks
    // as readily as it grows (B4 retired the per-bus instantiation that made a
    // shrink a rewiring problem).
    void setChannelCount( int nChannels );

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

    // --- the instrument slot (proposal 37 P3b) ------------------------------
    //
    // A slot is an instrument because its DESCRIPTOR says so, which is what
    // keeps a track looking like an instrument track on a machine where the
    // plugin is not installed (the placeholder reports the declared event shape
    // for the same reason it reports the declared I/O).
    bool isInstrument() const { return descriptor_.isInstrument; }

    // THE FEED. The track hands its twEventSource (its own event clip set
    // merged with the feeds of the children that bubble up, design 3.2.1) to
    // slot 0 when slot 0 is an instrument, and takes it away from every other
    // slot. The processor cannot tell a merge from a plain clip set.
    void setEventSource( std::shared_ptr<const twEventSource> source );
    // The transport half of twProcessContext, from the project's tempo map.
    void setTempoMap( const twTempoMap &map );

    // How long the plugin keeps producing after its last event. 0 when the slot
    // has no processor yet - this NEVER materializes one, because it is read
    // from STrack::getDuration(), which a render thread may call and which must
    // not be able to instantiate a plugin.
    std::uint32_t tailFrames() const;

    // Forget the processor's page continuity (design D4). The P3c run barrier
    // calls this on every instrument slot before a render or a play start: an
    // epoch bump does NOT clear lastEnd_, so a run whose first page starts
    // exactly where the previous one stopped would continue its voices instead
    // of chasing them.
    void forgetContinuity();

    // --- automation (proposal 37 P5, design D5 / §4.5) ----------------------
    //
    // A slot owns its `param:<id>` lanes. Their VALUE domain is the plugin's
    // HOST-FACING one — native for CLAP/AU, normalized [0,1] for VST3 (plugins
    // inv. 26) — i.e. exactly what set-plugin-param writes and getParam()
    // returns, so a lane and a static edit are in the same units by
    // construction.
    virtual void onAutomationChanged( SAutomationLane &lane,
                                      offset_t start, offset_t end ) override;
    virtual void applyAutomationToEngine() override;

signals:
    void bypassChanged( bool );
    // The slot was re-instantiated (reloadPlugin()): its state/mode may have
    // changed and any editor showing its parameters is stale. M5 listens.
    void pluginReloaded();
    // A parameter (or a whole state chunk) changed underneath an open editor —
    // emitted by notifyPluginEdited() and restoreState(). SPluginParamEditor
    // re-reads its sliders from the plugin on this, which is what makes UNDO of
    // a set-plugin-param visible in an editor that is already on screen (the
    // action mutates the plugin, not the widget).
    void paramsChanged();
    // "What I produce changed; stale the pages ABOVE me." The owning STrack
    // listens (STrack::onPluginSlotAudioInvalidated).
    //
    // A slot cannot do this itself, and proposal 08 M3 assumed it could.
    // SObject::invalidateRenderPath() walks DOWN from the project root through
    // childLinks() looking for `this` and bumps the render-chain epoch of every
    // container that contains it — but a track's SPluginChain is deliberately
    // NOT an SLink child of the track (STrack's constructor: "we do NOT call
    // setParent(this)"), so the walk never reaches a slot, nothing is found,
    // and NOTHING is invalidated. That is why a bypass toggle and a parameter
    // edit were still inaudible after M3: the slot's own tap pages were staled,
    // the twPluginChain / twTrackMix / mixer pages above them were not, and the
    // render served the audio it had already produced.
    void audioInvalidated();
    // The RANGE-SCOPED twin (proposal 37 P5): an automation edit at `start`
    // stales [start, end) rather than the whole chain. `end` is INT64_MAX for a
    // parameter lane, because a plugin is CLASS 1 — its state at any position
    // depends on everything before it, so an edit at `a` can change every page
    // after it (design F9). The signal exists for the same reason
    // audioInvalidated() does: SPluginChain is deliberately not an SLink child
    // of its track, so SObject::invalidateRenderPathRange() from here is a
    // NO-OP and the slot must ask its track to do the walk.
    void audioInvalidatedRange( qint64 start, qint64 end );

private:
    // Materialize the processor (once) and grow the tap vector to nBuses.
    void ensureChannels( int nChannels ) const;
    // bumpContentEpoch-style invalidation for everything above this slot: emits
    // audioInvalidated() AND calls SObject::invalidateRenderPath(), which is a
    // no-op today but stays correct if the chain is ever linked into the tree.
    void invalidateAbove();
    // (Re-)resolve effective_ from the registry; falls back to descriptor_ with
    // its module path resolved.
    void resolveEffective();
    audio::twPluginSlotProcessor::Factory makeFactory() const;
    // Collect every `param:` lane into the processor's curve map.
    void pushParamCurves();

    audio::twPluginDescriptor descriptor_;   // as stored / as given (VERBATIM)
    audio::twPluginDescriptor effective_;    // as resolved against the registry

    std::shared_ptr<audio::twPluginSlotProcessor>  proc_;
    std::shared_ptr<audio::twPluginInsert>        insert_;   // ONE, N channels wide
    int  channels_ = 0;
    bool bypass_ = false;
    std::vector<std::uint8_t> savedState_;  // opaque plugin state chunk
};

#endif
