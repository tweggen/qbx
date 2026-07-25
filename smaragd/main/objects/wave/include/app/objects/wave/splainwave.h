
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
    SPlainWaveRendererInline *inlineRenderer_;
    std::shared_ptr<std::atomic<bool>> analyzing_;

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

