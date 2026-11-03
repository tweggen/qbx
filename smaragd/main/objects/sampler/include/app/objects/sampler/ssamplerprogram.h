#ifndef _SSAMPLERPROGRAM_H_
#define _SSAMPLERPROGRAM_H_

#include <QList>
#include <QPointer>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "app/model/sobject.h"
#include "tw/sampler/twsamplerprogrammodel.h"
#include "tw/sampler/twsamplerstretch.h"

class QDomElement;
class QTextStream;
class SObjectRenderer;
class SProject;
class SProjectLoader;
class QTimer;
class SLink;
class twComponent;

namespace audio {
class twPlugin;
class twSamplerProgram;
class twSamplerSample;
}

/**
 * SSamplerProgram — the model object of ONE NassauDigital instance (proposal
 * 56 §4.4, Q1; nassau-digital DESIGN §9.2).
 *
 * It WRAPS THE CORE'S PROGRAM: the truth is an audio::twSamplerProgramModel
 * (tw/sampler's pimpl over nd::Program), so there is one model, Qt-free inside,
 * and this object adds what the app needs outside — undo (through the verbs in
 * ssampleractions.h), persistence, signals, and the hand-over to the instrument.
 * qbx never reads a program attribute itself (proposal 56 invariant 8).
 *
 * WHERE IT LIVES (B4): an SObject child of the SPluginSlot that hosts the
 * instance, through the slot's SLink-children loop. objects/sampler does NOT
 * depend on objects/track (Q-6, answered 2026-10-27): the slot only knows it
 * has an SObject child, and the SHELL — the one module that sees both — finds
 * the program for a slot (ssamplerslots.h) and hands the slot this object's
 * instanceInitializer() (setInstaller below).
 *
 * HOW AN EDIT REACHES THE INSTRUMENT: every committed edit makes a new
 * immutable snapshot and hands an initializer that installs it to the bound
 * installer, which (shell) sets it as the slot's instance initializer — so a
 * REBUILT instance is born current (invariant 4) — and applies it to the
 * instances that exist, under the processor's mutex (invariant 3, §4.3).
 *
 * THE SELECTION (select-zones) is model state of this object, NOT of the core
 * program: it changes no sound, installs nothing, and is NOT saved. ASSUMPTION
 * awaiting the author (proposal 56 §11, Q1): DESIGN.md and the proposal say
 * select-zones is an undoable action and no more.
 *
 * A PROGRAM THE CORE REFUSES ON LOAD (a value out of range from a newer build,
 * say) is kept VERBATIM and written back unchanged; the instance plays the
 * empty program and every edit is refused, announced. Losing the user's
 * program on the next save would be worse than not playing it.
 *
 * THE FILE: `<SSamplerProgram id='…' …><program><![CDATA[<NassauDigitalProgram
 * …>…</NassauDigitalProgram>]]></program></SSamplerProgram>`. The core's element
 * is nested verbatim as CHARACTER DATA rather than as child elements, because
 * the project file is read with QDom, and QDom does not keep attribute order —
 * QDomElement::save() writes them SORTED BY NAME (measured, Qt 6): handed back
 * to the core through QDom, a program with two unknown attributes on one
 * element would come back REORDERED, which the core's structural equality
 * (Extras in document order) calls a different program. As text, the core
 * sees exactly what it wrote (GATES Q1.2, proposal 56 §11 Q1).
 *
 * GENERATIONS (proposal 56 §4.6, Q2; DESIGN §6.1). A layer whose `src` is
 * `file:<path>` gets its PCM from a GENERATION JOB: a worker std::thread
 * (Qt-free, THREADING rule 1) decodes the file into an immutable
 * twSamplerSample stamped with the layer's next generation number. The main
 * thread POLLS for finished jobs (a QTimer, the pumpLocator pattern: workers
 * store, the main thread reads) and INSTALLS the result: one installer call,
 * so the slot's instance initializer carries every current generation and the
 * instances that exist get the new buffer under the processor's mutex, with a
 * param-epoch bump and the path above the slot staled (DESIGN §6.1 rule 2).
 * WHILE AN OFFLINE RENDER RUNS a finished job is NOT installed: it waits for
 * the next poll after the render (rule 3, proposal 56 invariant 6). settle()
 * is the synchronous, bounded wait the run barrier makes before an offline
 * render (B8, invariant 7): it blocks the main thread on the jobs themselves,
 * never on the event loop, because the render verb sleeps on the main thread.
 * Any other `src` (`test:…`, empty) loads nothing.
 *
 * LIVE ASSET LINKS (proposal 56 §4.7, Q3; DESIGN §6.2). A layer whose `src` is
 * `link:<n>` plays a BOUNCE of a qbx asset. <n> names an ASSET LINK of this
 * object: an SLink child targeting the asset BODY (invariant 9), minted by
 * `set-layer src="asset:<name>"` and never reused, so the src text alone says
 * which body a layer plays and the text-based undo machinery (restore, the
 * core's zone/layer XML) carries a link exactly as it carries a file. A link
 * whose layer stops naming it stays in memory (an undo may name it again) and
 * is not saved. The link holds the body by object, so renaming the asset
 * changes nothing it holds. What triggers a bounce is the asset's OWN
 * content epoch: on every arrangementChanged each linked layer compares the
 * epoch (and the window) of the body's content component with the one its
 * current generation was bounced from; only a moved one schedules a bounce,
 * debounced. The bounce is a generation job like a file's (Job, below): a
 * worker freezes the content component's pages over the asset's window into
 * an immutable twSamplerSample, and the install, the render pin and the settle
 * are the file layer's.
 *
 * TEMPO MODES BY STRETCH (proposal 56 §4.8, Q4; DESIGN §5, §13). A layer of a
 * `tempo-loop` / `tempo-oneshot` zone whose method is `stretch` and whose
 * `beats` > 0 plays its source STRETCHED to the project tempo by qbx's phase
 * vocoder (twSamplerStretch), pitch kept: the same generation job decodes (or
 * bounces) the source and then stretches it, and the result carries the
 * factor as its time scale, which the core applies to the layer's positions.
 * The source is kept beside the generation, so a TEMPO CHANGE (bpmTempoChanged)
 * or an edit of the zone's mode / method or the layer's beats or positions
 * re-stretches it as a new generation without decoding again. The vocoder's
 * transient keyframes come from the source's ONSETS: looked up in the sidecar
 * store by the source's content hash, else computed by an ANALYSIS job this
 * object starts itself on the kept source (a std::thread, like a generation
 * job; the result is stored in the sidecar store); its completion re-stretches
 * the layer once more as a new generation (DESIGN §5: "once more when qbx's
 * onset analysis of the source finishes"). The barrier's settle waits for
 * generation jobs, never for an analysis.
 */
class SSamplerProgram : public SObject {
    Q_OBJECT

public:
    using InstanceInitializer = std::function<void( audio::twPlugin & )>;
    // What the shell binds. `future` is the slot's next instance initializer:
    // it installs the current program AND every current generation, so a
    // rebuilt instance is born current (invariant 4). `now` is applied to the
    // instances that exist (applyToInstances) and carries only what changed:
    // the program after an edit, the new buffers after an install.
    using Installer = std::function<void( const InstanceInitializer &future,
                                          const InstanceInitializer &now )>;

    explicit SSamplerProgram( SProject *project );
    ~SSamplerProgram() override;

    // --- SObject: it produces no audio and is never placed on a lane ---------
    std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override { return nullptr; }
    // No time extent: a program is not material on a timeline.
    bool     hasDuration() const override { return false; }
    length_t getDuration() const override { return 0; }

    int serialize( QTextStream &o ) override;
    static SLink *instantiateFromDomElement( SProjectLoader &loader, QDomElement &element,
                                             SObject *parent );

    // --- the program ------------------------------------------------------------
    const audio::twSamplerProgramModel &model() const { return model_; }
    // False when the stored program could not be read; then loadError() says
    // why, every edit is refused and the stored text is written back unchanged.
    bool    isReadable() const { return loadError_.isEmpty(); }
    QString loadError() const { return loadError_; }

    // The ONE way to change the program. `fn` edits a copy; when it returns
    // true the copy replaces the program and is published (one snapshot, one
    // install); when it returns false nothing changed. Refused (false, and
    // `why`) on an unreadable program.
    bool edit( const std::function<bool( audio::twSamplerProgramModel & )> &fn,
               QString *why = nullptr );

    // --- the selection (select-zones) ----------------------------------------------
    // The ids that are selected AND exist, in selection order. A removed zone
    // drops out by itself and comes back with its undo.
    QVector<std::uint32_t> selectedZones() const;
    // The stored list, ids that no longer exist included (an inverse restores it).
    QVector<std::uint32_t> rawSelection() const { return selection_; }
    void setSelection( const QVector<std::uint32_t> &ids );

    // --- the instance's view, its pad and its record input (Q6) ------------------
    // MODEL STATE OF THIS OBJECT, OUTSIDE THE CORE'S PROGRAM (the author's
    // answer of 2026-11-02, nassau-digital DESIGN §13): saved in qbx's own
    // <SSamplerProgram> element, never in the program text, so an exported
    // program never carries them. Each is written only when it is not the
    // default, so every earlier file is byte-unchanged. Changed by the verbs
    // set-sampler-view and set-sampler-record-input (ssamplerrecordactions.h);
    // they change no sound and publish nothing.
    enum class View { Key, Pad, Map, Zone };
    // THE PADS (DESIGN §7; §13 "Settled on 2026-11-03, pads", QBX-277). Pads
    // are banks of 16: pad p of bank b is pad n = 16·b + p over all banks,
    // which the CORE maps to a key — its row in the program's <Pads> table,
    // else its default 36 + n. A pad names a key, not a zone; its zone is
    // the zone covering that key. Banks 0..7 (the core's 128 pads); a pad
    // EXISTS when it has a key, so without a table row bank 5 holds pads
    // 0..11 and banks 6 and 7 hold none (their defaults would pass 127).
    static constexpr int kPadDefaultBaseKey = 36;
    static constexpr int kPadsPerBank       = 16;
    static constexpr int kPadBanks          = 8;
    static int  padNumberOf( int bank, int pad ) { return kPadsPerBank * bank + pad; }
    static bool padInRange( int bank, int pad )
    {
        return bank >= 0 && bank < kPadBanks && pad >= 0 && pad < kPadsPerBank;
    }
    // In range AND the program gives the pad a key.
    bool padExists( int bank, int pad ) const
    {
        return padInRange( bank, pad ) && model_.padKey( padNumberOf( bank, pad ) ) >= 0;
    }
    static QString viewText( View v );
    static bool    viewFromText( const QString &text, View &out );

    View view() const { return view_; }
    int  padBank() const { return padBank_; }
    int  pad() const { return pad_; }
    int  padNumber() const { return padNumberOf( padBank_, pad_ ); }
    // The selected pad's key, from the program's table (-1: the pad has none,
    // which only an undone set-pad-key on a pad past the defaults can leave).
    int  padKey() const { return model_.padKey( padNumber() ); }
    // Callers validate (padExists); a bank or pad out of range is not stored.
    void setViewState( View view, int padBank, int pad );
    // A NOTE FROM THE DEVICE INPUT SELECTS THE PAD MAPPED TO ITS KEY (DESIGN
    // §13, 2026-11-03), switching bank when the pad is in another: when
    // several pads share the key the selected pad stays if it is one of them,
    // otherwise the lowest-numbered one (the core's padSelectedByKey). A key
    // no pad plays changes nothing. Live, from SSamplerKeyTaps, not an undo
    // step; the selection it leaves is saved like any other. True when the
    // selection changed.
    bool selectPadByKey( int key );

    // Q-1 (answered 2026-11-01): the instance's OWN record input, spelled as a
    // track input is, `audio:<device>:<mask>` (<mask> hex), or empty for none.
    // An armed track holding this instance records INTO it from this input,
    // whatever the track's own input is (shell's SAudioRecorder).
    const QString &recordInput() const { return recordInput_; }
    void setRecordInput( const QString &spec );
    // `audio:<device>:<mask>` -> <device> (may be empty: the app's default).
    QString  recordInputDevice() const;
    unsigned recordInputMask() const;
    // Is `spec` a record input this object takes? Empty (none) or
    // `audio:<device>:<mask>` with a non-zero hex mask.
    static bool isValidRecordInput( const QString &spec );

    // THE LAST KEY PLAYED (DESIGN §7): the last note-on the instance's DEVICE
    // MIDI INPUT TAP saw (the tap Q7.2's hand selection uses, not the
    // engine's event feed; the shell's SSamplerKeyTaps owns it). Live state:
    // -1 until a key is played, NOT saved and not undoable.
    int  lastKey() const { return lastKey_; }
    void setLastKey( int key );

    // --- the hand-over to the instrument -----------------------------------------
    InstanceInitializer instanceInitializer() const;
    // Bind: store the installer and publish the current program through it at
    // once. An empty installer unbinds.
    void setInstaller( Installer installer );
    bool isBound() const { return static_cast<bool>( installer_ ); }
    // The serial of the snapshot most recently published (0 = none yet): what an
    // instance's twSamplerHostStats::installedSerial must equal.
    std::uint64_t publishedSerial() const { return publishedSerial_; }

    // --- generations (proposal 56 §4.6, Q2) ----------------------------------------
    struct LayerState {
        std::uint64_t installedGeneration = 0;  // 0 = no buffer installed
        QString       installedSrc;             // the src that buffer came from
        std::uint64_t pendingGeneration   = 0;  // 0 = no job outstanding
        QString       pendingSrc;
        // A linked layer whose asset moved, waiting for its debounce (Q3).
        bool          bounceDue = false;
        // DESIGN §6.2's "rendering" mark: a job outstanding, or a bounce due.
        bool          rendering() const { return pendingGeneration != 0 || bounceDue; }
        int           installs = 0;             // generations installed so far
        int           renders  = 0;             // asset bounces STARTED so far (Q3.2)
        // Q4: the installed buffer's time scale (the stretch factor; 1 when
        // not stretched), the onset keyframes it was stretched with (-1 = it
        // was not stretched with an onset list: unstretched, or the vocoder's
        // fixed grid), an onset analysis running, and analyses finished.
        double        timeScale = 1.0;
        bool          stretched = false;
        int           onsets    = -1;
        bool          analyzing = false;
        int           analyses  = 0;
    };
    LayerState layerState( std::uint32_t layerId ) const;
    // Jobs started and not yet installed (finished ones a render kept back
    // included).
    int outstandingGenerations() const;

    // THE BARRIER'S SETTLE (B8): wait, on the calling (main) thread, for every
    // outstanding job, at most `boundMs` in all, and install what finished.
    // True when nothing is outstanding afterwards; false with `why` when a job
    // did not finish inside the bound (what finished is installed anyway).
    // `waitedFor` = how many generations were outstanding, `waitedMs` = how
    // long it took.
    bool settle( int boundMs, int *waitedFor, qint64 *waitedMs, QString *why );

    // TEST SEAM (set-sampler-render-gate): the NEXT job started for `layerId`
    // holds before it decodes until release, or until `holdMs` ms have passed
    // when holdMs >= 0. release() also frees a job that is holding now.
    void holdNextJob( std::uint32_t layerId, int holdMs );
    void releaseHeldJob( std::uint32_t layerId );
    // The same seam for the layer's NEXT onset analysis (Q4.2): it holds before
    // it analyses, until released or for holdMs.
    void holdNextAnalysis( std::uint32_t layerId, int holdMs );
    void releaseHeldAnalysis( std::uint32_t layerId );
    // Wait (bounded, on the calling main thread) for every onset analysis of
    // this program, then act on what finished: each one that changes a layer's
    // keyframes starts that layer's re-stretch (a new generation, which a
    // following settle() waits for). False with `why` on the bound.
    bool waitAnalyses( int boundMs, QString *why );

    // --- asset links (proposal 56 §4.7, Q3) -----------------------------------------
    // Mint a link to `body` (a registered asset's body) and return its id, the
    // <n> of `link:<n>`. `reuse` != 0 hands back that id when it is this
    // object's link to the same body (a redo re-applies the forward action and
    // must name the link it made the first time); otherwise a new id. Never 0.
    std::uint32_t linkAsset( SObject *body, std::uint32_t reuse );
    // Drop link <n> again when no layer names it (a refused set-layer).
    void forgetLink( std::uint32_t linkId );
    // The body link <n> targets, or null when there is no such link.
    SObject *linkTarget( std::uint32_t linkId ) const;
    // The bodies the layers link NOW (each once): what the cycle walk follows.
    QList<SObject *> liveLinkTargets() const;
    // Start, now, every bounce a moved asset made due (the debounce cut
    // short). settle() calls it first; so does the shell before it asks
    // outstandingGenerations().
    void flushDueBounces();
    // The debounce between an asset's epoch moving and its bounce starting.
    static int bounceDebounceMs();

    struct Job;    // Qt-free state shared with one worker (ssamplerprogram.cpp)
    struct Gate;
    struct Source;         // a kept, unstretched source (Q4)
    struct AnalysisJob;    // one onset analysis on its worker (Q4)
    // What a layer's generation should be (Q4): stretched to `spec`, or not.
    struct Want {
        bool                        on = false;
        audio::twSamplerStretchSpec spec;
        bool operator==( const Want &o ) const { return on == o.on && ( !on || spec == o.spec ); }
        bool operator!=( const Want &o ) const { return !( *this == o ); }
    };

signals:
    void programChanged();
    void selectionChanged();
    void viewChanged();
    void recordInputChanged();
    // A generation was installed into the instrument (or a layer's buffer was
    // removed); `layerId` names it.
    void generationInstalled( quint32 layerId, quint64 generation );

private:
    void publish_();
    void reconcileSamples_( bool atBind = false );
    void startJob_( std::uint32_t layerId, const QString &src,
                    std::shared_ptr<const Source> from = nullptr );
    void cancelJob_( std::uint32_t layerId );
    // Install every finished job (`force` ignores the render pin: settle()).
    // Returns how many generations were installed.
    int  installFinished_( bool force );
    void installChanged_( const std::vector<std::uint32_t> &layers );
    void pollJobs_();
    // The asset-link half (Q3).
    struct LinkKey;
    bool linkKeyOf_( const QString &src, LinkKey &out, QString *why ) const;
    void startBounce_( std::uint32_t layerId, const QString &src, const LinkKey &key );
    // Compare every linked layer with its asset; `start` starts the due
    // bounces now, otherwise they are marked due and the debounce (re)armed.
    void checkLinks_( bool start );
    void onArrangementChanged_();
    void ensurePolling_();
    InstanceInitializer futureInitializer_() const;
    // The stretch half (Q4).
    Want wantFor_( std::uint32_t layerId ) const;
    std::shared_ptr<const std::vector<std::uint64_t>> knownOnsets_( const Source &s ) const;
    void checkStretch_();
    void maybeAnalyze_( std::uint32_t layerId );
    int  processAnalyses_();
    void onTempoChanged_();

    audio::twSamplerProgramModel                  model_;
    QString                                       loadError_;
    QString                                       rawText_;     // verbatim, when unreadable
    QVector<std::uint32_t>                        selection_;
    View                                          view_    = View::Key;
    int                                           padBank_ = 0;
    int                                           pad_     = 0;
    QString                                       recordInput_;
    int                                           lastKey_ = -1;
    std::shared_ptr<const audio::twSamplerProgram> snapshot_;
    std::uint64_t                                 publishedSerial_ = 0;
    Installer                                     installer_;

    struct Installed {
        std::uint64_t                                 generation = 0;
        QString                                       src;
        std::shared_ptr<const audio::twSamplerSample> sample;
        std::shared_ptr<const LinkKey>                key;   // a bounce's, else null
        // Q4: the generation was made for `want`; `source` is the unstretched
        // source kept for a re-stretch and an analysis (only when want.on);
        // `stretched` = the vocoder ran (factor != 1); the onsets it used.
        Want                                          want;
        std::shared_ptr<const Source>                 source;
        bool                                          stretched = false;
        std::shared_ptr<const std::vector<std::uint64_t>> onsets;
    };
    std::map<std::uint32_t, Installed>              installed_;
    std::map<std::uint32_t, std::uint64_t>          lastGeneration_;  // never reset
    std::map<std::uint32_t, int>                    installs_;
    std::map<std::uint32_t, std::shared_ptr<Job>>   jobs_;            // outstanding
    std::vector<std::shared_ptr<Job>>               retiredJobs_;     // cancelled, to join
    std::map<std::uint32_t, std::shared_ptr<Gate>>  armedGates_;      // for the next job
    std::map<std::uint32_t, std::shared_ptr<Gate>>  liveGates_;       // held by a job now
    QTimer                                         *poll_ = nullptr;

    // A link is PINNED (an SLink child, saved) only while a layer names it.
    // One no layer names keeps only a WEAK pointer to its body, so an undo
    // can name it again but it holds nothing: a pin nobody uses would keep a
    // reference cycle (asset -> track -> slot -> program -> asset) alive after
    // its layer let go, and ~SProject cannot order a cycle (measured: a
    // teardown SEGFAULT, QBX-266).
    struct LinkRec {
        QPointer<SObject> body;
        SLink            *pin = nullptr;
    };
    std::map<std::uint32_t, LinkRec>                links_;
    std::set<std::uint32_t> namedLinks_() const;
    void syncLinkPins_();
    std::uint32_t                                   nextLinkId_ = 1; // never reused
    std::map<std::uint32_t, int>                    renders_;
    std::set<std::uint32_t>                         due_;            // bounce waiting
    QTimer                                         *debounce_ = nullptr;

    // Q4: onset lists by source content (from the sidecar or an analysis),
    // the running analyses, their gates, and how many finished per layer.
    std::map<std::pair<std::uint64_t, std::uint64_t>,
             std::shared_ptr<const std::vector<std::uint64_t>>> onsets_;
    std::map<std::uint32_t, std::shared_ptr<AnalysisJob>>  analysisJobs_;
    std::vector<std::shared_ptr<AnalysisJob>>              retiredAnalyses_;
    std::map<std::uint32_t, std::shared_ptr<Gate>>         armedAnalysisGates_;
    std::map<std::uint32_t, std::shared_ptr<Gate>>         liveAnalysisGates_;
    std::map<std::uint32_t, int>                           analyses_;
};

#endif
