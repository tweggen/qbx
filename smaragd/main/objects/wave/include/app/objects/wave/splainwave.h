
#ifndef _SPLAINWAVE_H
#define _SPLAINWAVE_H

#include <atomic>
#include <memory>
#include <vector>

#include "app/model/sobject.h"
#include "app/model/sexternfile.h"
#include "tw/sidecar/twanalyzers.h"   // twOnset (objects/wave has the sidecar grant)

class twComponent;
class twRandomSource;
class twWavInput;

class SObjectRenderer;
class SPlainWaveRendererInline;
class SProjectLoader;

/**
 * A Plainwave object is an audio source.
 * Being one of the extern file objects, it is kept inside the project.
 *
 * Thread affinity: MIXED (not thread-safe)
 * - cpWave_: accessed from UI thread (getPreview) AND audio thread (getRootComponent→calcOutputTo)
 * - fileName_: read from UI thread only
 * - inlineRenderer_: UI thread only
 * - previewData_: accessed from UI thread only
 *
 * RACE CONDITION: cpWave_->file_ (QFile) is accessed from both threads without synchronization.
 * Execution paths:
 *   UI:    paintEvent → draw() → getPreview() → getStraightPreview() → straightCalcPreviewData()
 *   Audio: callback → calcOutputTo() → cpWave_->calcOutputTo() → file_.seek/read()
 */
class SPlainWave
    : public SExternFile
{
    Q_OBJECT
public:
    SPlainWave( SProject *project );
    virtual ~SPlainWave();
    
    static SLink *instantiateFromDomElement( SProjectLoader &projectLoader, 
					     QDomElement &element, 
					     SObject *parent );

    virtual std::shared_ptr<twComponent> getRootComponent();
    virtual twRandomSource *getRandomSource();
    virtual int setWave( const QString url );
    virtual QString getFileName() const;
    virtual bool relocateTo( const QString &newPath ) override;
    virtual bool isMissing() const override { return missing_; }

    /**
     * Become a MISSING PLACEHOLDER for a sample that could not be loaded.
     *
     * WHY THIS EXISTS AT ALL. Before it, a `<SPlainWave>` whose file was
     * unreachable returned NULL from instantiateFromDomElement, and the loader
     * — correctly, given that — dropped the element AND cascaded the drop to
     * every `<SCut>` that windowed it, "so the rest of the project can load".
     * The cost was silent and permanent: a project opened on a machine where
     * one sample happens to be absent came up MINUS those clips, and the next
     * save wrote that arrangement back. A file being temporarily out of reach
     * (another machine, an unsynced cloud folder, an external disk) is a normal
     * condition; losing the arrangement over it is not.
     *
     * A placeholder keeps the object's identity — the path AS THE FILE SPELLS
     * IT, so a re-save says exactly what the original said and the reference
     * resolves again on the machine where the file does exist — plus the
     * duration the project recorded, so the clips keep their extents and the
     * timeline is unchanged. It owns a source-less twWavInput, so it is a
     * perfectly ordinary component that renders SILENCE: the graph, the
     * capture builder and the freeze path need no missing-file branch anywhere.
     *
     * `durationFrames` comes from the element's own `durationSec` (there is no
     * file to ask). Zero when the attribute was absent — the clips then keep
     * their own windows, which is all a cut actually reads.
     */
    int setMissingWave( const QString &resolvedPath, const QString &storedSpelling,
                        length_t durationFrames );

    /**
     * Build (or reuse) the MISSING placeholder for `resolved` and return a link
     * to it. Mirrors SProject::linkToFile's caching: a second clip on the same
     * unreachable file shares the one placeholder, exactly as two clips on a
     * present file share one SPlainWave.
     */
    static SLink *linkToMissingFile( SProject &project, const QString &resolvedPath,
                                     const QString &storedSpelling,
                                     length_t durationFrames );

    // FIXME: Move this to a factory.
    virtual QWidget *getDetailEditWidget( QWidget *parent );
    virtual QWidget *getInlineEditWidget( QWidget *parent );
    virtual SObjectRenderer *getInlineRenderer();

    virtual bool hasDuration() const;
    virtual length_t getDuration() const;
    
    virtual bool hasPreview() const;
    virtual int getPreview( preview_t *dest,
			    offset_t start, length_t length,
			    offset_t nProbes );

    // Proposal 27 M1: true while a background analysis job for this wave's
    // content is queued or running. Lock-free (paint-time badge read). The
    // flag is shared with the job closure via shared_ptr, so a wave deleted
    // mid-job leaves the closure a valid flag to clear.
    bool isAnalyzing() const {
        return analyzing_ && analyzing_->load( std::memory_order_acquire );
    }

    // Proposal 28 W2: onset marks for the clip painter. SOURCE-rate onset
    // positions (twOnset.pos) plus the sidecar's sourceRate, so the renderer
    // can rescale to project rate. Read lock-free at paint time (atomic
    // shared_ptr load), mirroring the isAnalyzing() badge-read pattern.
    struct UiOnsets {
        std::vector<twOnset> onsets;
        uint32_t             sourceRate = 0;   // rate the positions are in
    };
    // Lazily populated on the FIRST call from this wave's "onsets" sidecar
    // (UI thread only). A MISS caches an EMPTY vector so paint never re-hits
    // the store; the analysis job clears the cache on completion so the next
    // paint reloads fresh results. Returns null only before construction has
    // finished wiring the slot (defensive).
    std::shared_ptr<const UiOnsets> onsetsForUi() const;

    // Proposal 40 "Feel Flow" M1: OPT-IN groove analysis (groove.res /
    // groove.ev, tw/sidecar/twaspects.h) for this wave's content. Never
    // called from setWave() — unlike enqueueAnalysis()'s onsets/loudness/f0,
    // this one costs a full pendulum-ensemble pass and is meaningful only on
    // drum/rhythmic material, so a caller (the feel-flow-analyze verb; a
    // later Track Detail toggle, M3) opts a clip in explicitly. Same
    // closure-lifetime discipline as enqueueAnalysis(): no-op when the
    // revalidator or the store is disabled, both aspects already validate,
    // or this wave has no content yet.
    void enqueueGrooveAnalysis();

    // Mirrors isAnalyzing() — true while a background groove-analysis job
    // for this wave's content is queued or running. A SEPARATE badge from
    // isAnalyzing(): groove analysis is a distinct, opt-in job with its own
    // lifetime, not folded into the always-on onsets/loudness/f0 pass.
    bool isAnalyzingGroove() const {
        return analyzingGroove_ && analyzingGroove_->load( std::memory_order_acquire );
    }

protected:
    virtual int serializeSelfAttributes( QTextStream &o );

    // Proposal 27 (M0): persist/restore the straight preview via the sidecar
    // store, keyed by the decoded content hash + project rate. UI thread only
    // (called from straightCalcPreviewData) — same affinity previewData_
    // always had, so the cpWave_->file_ race note above is untouched; a
    // sidecar hit actually AVOIDS the racy fallback reads entirely.
    bool fetchPreviewSidecar( preview_t *dest, offset_t nProbes,
                              offset_t skip, offset_t forLength ) override;
    void storePreviewSidecar( const preview_t *data, offset_t nProbes,
                              offset_t skip, offset_t forLength ) override;

private:
    // Proposal 27 M1: enqueue background sidecar analysis (onsets, loudness)
    // for this wave's content on the project's worker pool. No-ops when the
    // revalidator is disabled, the store is disabled, or the sidecars already
    // validate. Called from setWave() (load + import).
    void enqueueAnalysis();

    std::shared_ptr<twWavInput> cpWave_;
    QString fileName_;
    // Set by setMissingWave(): this object stands in for a file that could not
    // be loaded. See setMissingWave() for what a placeholder is and is not.
    bool    missing_ = false;
    // The spelling the PROJECT FILE used, kept verbatim so a re-save reproduces
    // it. Re-encoding fileName_ against this machine's anchor would be right by
    // accident at best: the reference is unresolvable HERE, so this machine has
    // no standing to rewrite where it points.
    QString storedSpelling_;
    // The duration the project recorded for the absent file, in project frames.
    length_t missingDuration_ = 0;
    SPlainWaveRendererInline *inlineRenderer_;
    std::shared_ptr<std::atomic<bool>> analyzing_;
    // Proposal 40 M1: the groove-analysis job's own badge (see
    // enqueueGrooveAnalysis()/isAnalyzingGroove()).
    std::shared_ptr<std::atomic<bool>> analyzingGroove_;

    // W2 UI onset cache. The SLOT is heap-allocated and shared with the
    // analysis-job closure by shared_ptr (analysis-lane lifetime rule: a wave
    // deleted mid-job leaves the closure a valid slot to clear). The pointer
    // inside is swapped with std::atomic_load/store, so onsetsForUi() (UI
    // thread) and the job's completion clear never tear.
    struct UiOnsetsSlot {
        std::shared_ptr<const UiOnsets> ptr;   // via std::atomic_load/store
    };
    std::shared_ptr<UiOnsetsSlot> uiOnsets_;
};

#endif

