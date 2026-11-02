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

signals:
    void programChanged();
    void selectionChanged();
    // A generation was installed into the instrument (or a layer's buffer was
    // removed); `layerId` names it.
    void generationInstalled( quint32 layerId, quint64 generation );

private:
    void publish_();
    void reconcileSamples_( bool atBind = false );
    void startJob_( std::uint32_t layerId, const QString &src );
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

    audio::twSamplerProgramModel                  model_;
    QString                                       loadError_;
    QString                                       rawText_;     // verbatim, when unreadable
    QVector<std::uint32_t>                        selection_;
    std::shared_ptr<const audio::twSamplerProgram> snapshot_;
    std::uint64_t                                 publishedSerial_ = 0;
    Installer                                     installer_;

    struct Installed {
        std::uint64_t                                 generation = 0;
        QString                                       src;
        std::shared_ptr<const audio::twSamplerSample> sample;
        std::shared_ptr<const LinkKey>                key;   // a bounce's, else null
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
};

#endif
