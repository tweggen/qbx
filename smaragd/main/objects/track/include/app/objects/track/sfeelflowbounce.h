#ifndef SFEELFLOWBOUNCE_H
#define SFEELFLOWBOUNCE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "tw/render/render_session.h"

class STrack;

// Proposal 40 "Feel Flow" M1b — the internal-bounce consumer for a TRACK's
// post-FX signal (design section 4.3, plan/proposed/40_GROOVE_RESONANCE.md).
//
// Read this before touching it — the analyzed object is deliberately NOT the
// track's live component graph and this holder deliberately does NOT ride
// SApplication::startRender(): that path blocks the UI, suspends every live
// lane and drains the analysis lane THIS job runs on. Instead:
//
//   1. start() spawns a background audio::RenderSession over the track's own
//      root component (STrack::getRootComponent() — post-FX, post-gain,
//      pre-summing) covering the WHOLE PROJECT DURATION, writing a float32
//      WAV into machine-local derived data BESIDE the sidecar store root
//      (twSidecarStore::instance().rootPath().parent_path() / "bounce" — a
//      SIBLING directory, never inside the store: bounce files are not QAF
//      sidecars). Its per-page memory is bounded by RenderSession::
//      setPruneScope() over the track's own chain components — trackMix,
//      pluginChain, gainStage, rewire, PLUS every slot's own twPluginInsert
//      (measured: a twPluginChain forwards its cache to its LAST INSERT
//      rather than holding pages itself, so an inserted plugin is a FIFTH
//      per-track cache the four-component design text names would silently
//      miss) — never the export path's global releaseOldPagesGlobally()
//      walk, which would prune a concurrently PLAYING graph's page trail at
//      the bouncer's (unrelated) position.
//
//      NOT in scope, measured and left as an open gap rather than a silent
//      one: a per-CLIP reader/grain component (twSampleReader and friends)
//      is not reachable from STrack the way the chain components are, so it
//      is never pruned by a bounce — over a single ~16 s fixture that holds
//      its whole ~12-page set resident for the run's lifetime, harmless; for
//      a multi-minute track it would grow unboundedly. See
//      feel_flow_bounce_stale.qxa's report-page-memory calls for the
//      measured numbers this note is based on.
//   2. On completion (on the render thread) it schedules an ANALYSIS job on
//      the project's CaptureRevalidator — the SAME lane
//      SPlainWave::enqueueGrooveAnalysis() uses, so a concurrent RENDER's
//      pauseBackground() still quiesces it. That job decodes the bounce WAV
//      with a bare twSampleSource (never an SPlainWave — the bounce must
//      NEVER appear in the project's extern-file list) and calls the same
//      pure twGrooveBuildAspectPayloads() + twSidecarStore::store() pair
//      enqueueGrooveAnalysis()'s own closure does, keyed by the BOUNCE's own
//      content hash.
//
// Staleness is the SCut::contentEpochForCapture_ pattern verbatim: the
// track root's contentEpochNow() is snapshotted at the START of the bounce;
// isStale() is a plain READ comparing it against the CURRENT epoch — nothing
// here ever bumps one. A stale result is never silently served as fresh.
//
// Not an SObject, not an SLink, not undoable, not serialized — derived data
// with no independent project existence, the same standing as an
// automation lane's snapshot curve.
class SFeelFlowTrackBounce {
public:
    explicit SFeelFlowTrackBounce( STrack *track );
    ~SFeelFlowTrackBounce();

    SFeelFlowTrackBounce( const SFeelFlowTrackBounce & ) = delete;
    SFeelFlowTrackBounce &operator=( const SFeelFlowTrackBounce & ) = delete;

    // Starts (or restarts) a bounce+analyze pass. No-op, logged, when the
    // sidecar store is disabled (SMARAGD_SIDECAR_DIR=off), the project has
    // no revalidator (SMARAGD_REVAL_WORKERS=0), or a bounce for this track
    // is already running. Never blocks.
    void start();

    // True while the bounce render OR the analysis job that follows it is
    // in flight.
    bool isBouncing() const {
        return bouncing_ && bouncing_->load( std::memory_order_acquire );
    }

    // True once a bounce+analyze pass has completed at least once.
    bool hasResult() const {
        return haveResult_.load( std::memory_order_acquire );
    }

    // True when the track's root-component content epoch has moved since
    // the START of the last successful bounce, or no bounce has ever
    // completed.
    bool isStale() const;

private:
    STrack *track_;   // not owned; this holder is owned BY the track
    std::unique_ptr<audio::RenderSession> session_;
    std::shared_ptr<std::atomic<bool>> bouncing_;
    std::atomic<bool> haveResult_{ false };
    std::atomic<uint64_t> epochAtBounce_{ 0 };
    std::string bouncePath_;
};

#endif // SFEELFLOWBOUNCE_H
