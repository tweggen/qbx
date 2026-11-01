#ifndef _SPLUGINSLOT_H_
#define _SPLUGINSLOT_H_

#include "app/model/sobject.h"
#include "tw/events/tweventsource.h"
#include "tw/events/twtempomap.h"
#include "tw/plugins/twplugindescriptor.h"

#include <functional>
#include <memory>
#include "tw/plugins/twpluginslotproc.h"
#include <QPointer>
#include <QString>
#include <QVector>
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
 * The DSP side of a slot is ONE audio::twPluginInsert component, N channels
 * wide, in front of ONE audio::twPluginSlotProcessor that owns the twPlugin
 * instance(s), the block chunking and the channel-mismatch mapping. See the
 * `pluginInsert`/`processor` members below, and proposal 36 B4.
 *
 * It used to be "one processor plus one twPluginInsert TAP PER BUS, because the
 * engine's one-mono-page-per-component model requires 1-in/1-out components".
 * That was true and load-bearing until proposal 36 B4 gave the frozen page a
 * channel dimension; a plugin now sees all its channels in one process() call
 * from one component, and the tap fan-out, the sideways sibling gather and the
 * processor's all-bus cache retired together. The processor stayed, as the
 * plugin's LIFETIME and STATE holder only (proposal 08 inv. 18 depends on a
 * slot's graph identity being its processor).
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

    // One row per parameter the LIVE plugin declares, in the plugin's own
    // order, in APP types. It exists so a caller that may not include
    // tw/plugins (app/timeline: the automation lane picker and the lane's value
    // scale, tools/check_layering.py) can still ask what this slot's knobs are
    // called and what range they span. Empty for a Missing/Unsupported slot.
    struct ParamRow {
        std::uint32_t id = 0;
        QString       name;
        double        minValue = 0.0;
        double        maxValue = 1.0;
        double        defaultValue = 0.0;
        bool          isStepped = false;
    };
    QVector<ParamRow> paramRows() const;

    /**
     * What the live plugin REPORTS as its processing latency, in frames
     * (proposal 21 L5). 0 for a Missing / Unsupported slot, and 0 for the many
     * plugins that report nothing.
     *
     * It is here for the same reason `paramRows()` is: the FX strip
     * (app/pluginui) and the transport readout have to show it, and the number
     * belongs to the plugin the slot owns. NOTHING COMPENSATES FOR IT - plugin
     * delay compensation is out of scope (proposal 37 P9) - so every mount that
     * shows it must also say that it is not compensated.
     */
    std::uint32_t reportedLatencyFrames() const;

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

    // Whether this slot's NATIVE EDITOR was open when the project was saved
    // (proposal 33 D2). It travels in the .qxp because "this project opens with
    // the synth's editor up" is a property of the arrangement and means the
    // same thing on any machine; the window's POSITION AND SIZE deliberately do
    // NOT — they live in SSettings, per user, keyed by plugin uid, because a
    // window at x=2400 is off screen on a laptop. Same split, and the same
    // reason, as proposal 37 P7's portable `midiOutPort` name against the
    // machine-local `midi/portId/<name>`.
    //
    // Written by SPluginNativeEditor and by nothing else. It is NOT an action:
    // opening a window is not an edit to the arrangement, and an undo stack
    // that has to be walked past a window-open entry to reach a real edit is
    // worse than a project flag that is not undoable.
    void setEditorOpen( bool open ) { editorOpen_ = open; }
    bool getEditorOpen() const { return editorOpen_; }

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

    // --- non-parameter state of a live instance (proposal 56 B3, §4.3) -------
    //
    // Both are GENERIC — neither names the sampler — and both exist because a
    // plugin whose state is not a parameter list (NassauDigital's program and
    // samples, handed over through tw/sampler's twSamplerHost) has two ways to
    // be lost: a push through the bare plugin() pointer races the render, and a
    // REBUILD (setChannelCount(), reloadPlugin(), a rescan's setFactory())
    // re-instantiates every instance from the factory and drops whatever was
    // pushed into the old ones.
    //
    // THE INSTANCE INITIALIZER: every instance makeFactory() produces from now
    // on is passed through `init` BEFORE the processor sees it (inside the
    // rebuild, under the processor's mutex), so a fresh instance is BORN
    // CURRENT (proposal 56 invariant 4). Held by the factory through a shared
    // holder, so setting it after the processor exists still reaches the next
    // rebuild. It does NOT touch the instances that already exist: that is
    // applyToInstances(). An empty function clears it. UI thread.
    using InstanceInitializer = std::function<void( audio::twPlugin & )>;
    void setInstanceInitializer( InstanceInitializer init );
    bool hasInstanceInitializer() const { return initHolder_ && *initHolder_; }

    // THE LOCKED MUTATION of the instances that exist now:
    // twPluginSlotProcessor::applyToInstances() — under the processor's mutex,
    // with a param-epoch bump — followed by notifyPluginEdited(), which stales
    // the path above the slot that the processor cannot reach. Materializes
    // nothing: a slot with no processor yet has no instances, returns 0, and
    // gets its state from the initializer when it is built. UI thread.
    std::size_t applyToInstances( const std::function<void( audio::twPlugin & )> &fn );

    // --- the slot's MODEL CHILDREN (proposal 56 B4, Q1) -----------------------
    //
    // A plugin whose state is model data rather than a blob (NassauDigital's
    // program, objects/sampler's SSamplerProgram) keeps that data in an SObject
    // CHILD of its slot, serialized through the slot's SLink-children loop
    // (serialize() below) and attached again on load (instantiateFromDomElement).
    // GENERIC, like the two seams above: the slot never learns what the child is.
    //
    // adoptChild() links `child` under this slot. Every child that arrives --
    // adopted at run time or read back from a file -- is then handed to the
    // CHILD-ADOPTED HOOK, which the SHELL installs (the one module that sees
    // both this slice and the child's): that is where the child is bound to
    // the slot, e.g. given as its instance initializer, without objects/track
    // naming objects/sampler or the reverse (proposal 56 Q-6, answered
    // 2026-10-27). No hook installed = nothing is bound. UI thread.
    SLink *adoptChild( SObject &child );
    using ChildAdoptedHook = std::function<void( SPluginSlot &slot, SObject &child )>;
    static void setChildAdoptedHook( ChildAdoptedHook hook );

    // --- aux routing (proposal 55 D8/D9, M2) ---------------------------------
    //
    // THE SLOT ID: a stable number a return track's <auxSource slot=> names.
    // A slot INDEX would move under reorder-plugin, and the slot OBJECT does
    // not survive remove-plugin + undo (the inverse inserts a NEW slot), so a
    // return names the slot by this number and remove-plugin's inverse carries
    // it back (T5). 0 = never referenced; it is minted by ensureSlotId() when a
    // return first names the slot and written only when non-zero, so a project
    // that never routed an aux bus re-serializes byte-identically (T8).
    //
    // Minted RANDOM (63 bits), not max+1: a counter would hand a new slot the
    // number of a removed one that a return -- alive on the undo stack --
    // still names, and the return would silently re-attach to it (T6).
    quint64 slotId() const { return slotId_; }
    void    setSlotId( quint64 id ) { slotId_ = id; }
    quint64 ensureSlotId();

    // THE ROUTED MASK (55 D7 "the routed mask is state"): bit b = aux bus b
    // reaches a return track. DERIVED, never stored: SStdMixer's pass computes
    // it from the returns that name this slot and hands it down through
    // STrack::applyRoutedAuxMasks(). Kept here so a processor built LATER (a
    // slot still waiting for its width) is born with it. Returns whether the
    // mask changed.
    bool          setRoutedOutBuses( std::uint32_t mask );
    std::uint32_t routedOutBuses() const { return routedMask_; }

    // The plugin's audio OUTPUT buses as the LIVE instance reports them (bus 0
    // the main bus), falling back to the effective descriptor when the slot is
    // not Active. The live answer is preferred because an in-repo fixture
    // inserted by path was never scanned, so its descriptor knows no buses.
    // 0 = unknown. UI thread.
    int auxOutBusCount() const;
    int auxOutBusChannels( int bus ) const;

    // --- the SIDECHAIN (proposal 55 D5/D8, M4) -------------------------------
    //
    // A keyed slot names a KEY LANE and a tap point: `pre` = the lane's chain
    // output (after its inserts, before its fader), `post` = its fader output
    // (47 D3's two points, STrack::sendTapComponent). The default is pre, so
    // pulling the key lane's fader down does not stop the ducking.
    //
    // THE SOURCE IS NAMED BY IDENTITY (D8): a QPointer, so a deleted lane can
    // never be inherited by a later object at the same address (T6), written
    // to the file as `<sidechain source='<objectId>' tap='pre'/>` inside this
    // slot's element, and ONLY WHEN SET, so a project that never keyed a slot
    // re-serializes byte-identically (T8). A source that did not resolve on
    // load keeps the sidechain SET with a null source: the slot stays keyed,
    // fails closed (a silent key) and re-saves as source='0'.
    //
    // The model half only: setting it WIRES NOTHING. The mixer's one pass
    // (SStdMixer::rewireKeys, inside reconnectTracksToMixer) wires port 1 of
    // the insert through STrack::wireSlotKey() -- 47 D4 / 45 T3. Callers that
    // want the pass to run go through STrack::setPluginSlotSidechain().
    bool     hasSidechain() const { return scSet_; }
    SObject *sidechainSource() const { return scSource_.data(); }
    bool     sidechainPreFader() const { return scPre_; }
    void     setSidechain( SObject *source, bool preFader );
    void     clearSidechain();

    // The plugin's SIDECHAIN input bus (its bus 1) as the LIVE instance
    // reports it, falling back to the effective descriptor -- auxOutBusCount's
    // rule. 0 = the plugin has none.
    int sidechainBusChannels() const;

    // THE KEY WIRING, called by STrack::wireSlotKey (the pass) and nowhere
    // else. `component` is the key lane's tap component, or null to unwire.
    // Holds a reference to it while it is wired, so a key lane deleted from
    // the undo stack cannot leave port 1 reading a destroyed producer (47's
    // unwireSendLane lesson, paid for the key). Returns whether the live
    // wiring changed -- and with it this insert's pages (setKeySource stales
    // them), so the caller invalidates the path above the slot.
    bool applyKey( const std::shared_ptr<twComponent> &component );
    /// LIVE: port 1 is wired AND reads `component`.
    bool keyWiredTo( const twComponent *component ) const;
    bool keyWired() const;
    /// "keyChannels=C|busChannels=S|mapping=<name>" from the processor, or "".
    QString describeKey() const;
    /// The last announced key state (STrack::wireSlotKey's once-per-state rule).
    QString &keyAnnounceState() { return scState_; }

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
    // Emitted FIRST in ~SPluginSlot() -- before proc_ (and therefore the live
    // audio::twPlugin instance) or any other member is torn down. Found
    // 2026-08-23 chasing a crash: SPluginNativeEditor / SPluginEffectStrip's
    // generic editor used to connect to the QObject base's own destroyed()
    // signal to close their window when the slot dies, but destroyed() fires
    // from ~QObject(), which -- being the BASE class -- runs strictly AFTER
    // ~SPluginSlot()'s body and every member destructor. By then proc_'s
    // plugin is already gone, and closeEvent() -> twPluginEditor::detach()
    // touches a dangling extension pointer -- SIGSEGV, reproduced with a
    // three-line script (open a native editor, never close it, let the
    // project tear down). This signal is emitted from a place where the slot
    // -- and the plugin behind it -- are still fully alive, so a listener's
    // detach() is operating on a live instance.
    void slotDestroying();
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
    // Hand a newly attached model child to the child-adopted hook.
    void notifyChildAdopted_( SObject &child );

    audio::twPluginDescriptor descriptor_;   // as stored / as given (VERBATIM)
    audio::twPluginDescriptor effective_;    // as resolved against the registry

    std::shared_ptr<audio::twPluginSlotProcessor>  proc_;
    std::shared_ptr<audio::twPluginInsert>        insert_;   // ONE, N channels wide
    int  channels_ = 0;
    bool bypass_ = false;
    bool editorOpen_ = false;
    std::vector<std::uint8_t> savedState_;  // opaque plugin state chunk
    quint64       slotId_     = 0;   // see slotId()
    std::uint32_t routedMask_ = 0;   // see setRoutedOutBuses()
    // The sidechain (proposal 55 M4), see hasSidechain().
    QPointer<SObject>             scSource_;
    bool                          scSet_ = false;
    bool                          scPre_ = true;
    QString                       scState_;   // last announced key state
    std::shared_ptr<twComponent>  keyHold_;   // see applyKey()
    // The instance initializer, shared with every factory makeFactory() hands
    // out, so a later setInstanceInitializer() reaches the next rebuild.
    std::shared_ptr<InstanceInitializer> initHolder_ =
        std::make_shared<InstanceInitializer>();
};

#endif
