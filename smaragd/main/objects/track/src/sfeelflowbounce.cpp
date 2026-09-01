#include "app/objects/track/sfeelflowbounce.h"

#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sappcontext.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"

#include "tw/core/twlog.h"
#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"
// STrack::trackMixComponent()/pluginChainComponent()/gainStageComponent()
// return shared_ptr<twTrackMix>/twPluginChain/twGainStage, only FORWARD
// declared by strack.h — the weak_ptr<twComponent> conversion below needs
// the complete types to see the (public) inheritance.
#include "tw/mix/twgainstage.h"
#include "tw/mix/twtrackmix.h"
#include "tw/plugins/twpluginchain.h"
#include "tw/plugins/twplugininsert.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "tw/schedule/capture_revalidator.h"
#include "tw/sidecar/twaspects.h"
#include "tw/sidecar/twgrooveaspect.h"
#include "tw/sidecar/twbodyplant.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sources/twsamplesource.h"

#include <QMetaObject>
#include <QString>

#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Proposal 40 M3 AC 4: the track's EFFECTIVE analysis params -- what the
// bounce+analyze job must actually run with, and what its paramsHash must be
// keyed on for isStale() to notice a mode/trained-state change. A track that
// has never touched Feel Flow's mode gets a plain default-constructed
// twGrooveAnalysisParams (mode == Adaptive), which is BYTE-IDENTICAL to what
// M1/M1b/M2 always built here -- see twGrooveAnalysisParams::serialize's own
// doc for why that keeps every pre-M3 store key unchanged.
twGrooveAnalysisParams buildEffectiveGrooveParams( STrack *track )
{
    twGrooveAnalysisParams gp;
    if( track && track->feelFlowMode() == STrack::FeelFlowMode::Trained ) {
        if( const twGrooveTrainedStructure *ts = track->feelFlowTrainedStructure() ) {
            gp.mode    = twGrooveMode::Trained;
            gp.trained = *ts;
        }
        // else: Trained requested but learn-feel-flow has never run for this
        // track -- gp stays at its Adaptive default. twGrooveBuildAspect
        // Payloads would fall back to Adaptive scoring even if mode==Trained
        // reached it with an untrained structure (its own trainedHasRegion.
        // empty() guard); falling back HERE too keeps this hash matching
        // what actually gets computed, rather than minting a store key
        // nothing else will ever produce again.
    }
    return gp;
}

} // namespace

SFeelFlowTrackBounce::SFeelFlowTrackBounce( STrack *track )
    : track_( track ),
      uiCache_( std::make_shared<UiSlot>() )
{
}

SFeelFlowTrackBounce::~SFeelFlowTrackBounce()
{
    // Explicitly, and FIRST: RenderSession::~RenderSession() cancels and
    // JOINS its thread, so this line blocks until any in-flight bounce's
    // onComplete has fully run (see render_session.cc — onComplete fires
    // before the thread function returns). That is what makes it safe for
    // onComplete to touch the SProject/CaptureRevalidator raw pointers it
    // captures: STrack's destructor resets feelFlowBounce_ from INSIDE
    // ~SProject()'s body (see strack.h's member comment), which runs before
    // SProject's own revalidator_ member is torn down. Letting session_
    // destruct as an ordinary member (in declaration order, after
    // bouncing_/haveResult_) would risk the callback observing a
    // half-destroyed *this — resetting it here, first, avoids the question.
    session_.reset();
}

bool SFeelFlowTrackBounce::isStale() const
{
    if( !haveResult_.load( std::memory_order_acquire ) ) return true;
    if( !track_ ) return true;
    std::shared_ptr<twComponent> root = track_->getRootComponent();
    if( !root ) return true;
    if( root->contentEpochNow() != epochAtBounce_.load( std::memory_order_acquire ) )
        return true;

    // Proposal 40 M3 AC 4: param-aware staleness. Cheap (a handful of floats
    // and a hash, never I/O or a demand) -- safe to call from a UI read.
    twGrooveAnalysisParams liveParams = buildEffectiveGrooveParams( track_ );
    std::vector<uint8_t> liveBlob;
    liveParams.serialize( liveBlob );
    const uint64_t liveHash =
        twSidecarStore::hashParams( liveBlob.data(), liveBlob.size() );
    return liveHash != paramsHashAtBounce_.load( std::memory_order_acquire );
}

void SFeelFlowTrackBounce::start()
{
    if( !track_ ) return;
    if( isBouncing() ) return;   // a bounce for this track is already in flight

    if( !twSidecarStore::instance().enabled() ) {
        TW_LOGI( "feelflow", "[bounce] sidecar store disabled "
                             "(SMARAGD_SIDECAR_DIR=off); track bounce is a no-op" );
        return;
    }

    SProject *project = track_->getProjectSafe();
    if( !project ) return;
    CaptureRevalidator *reval = project->getRevalidator();
    if( !reval ) {
        TW_LOGI( "feelflow", "[bounce] no revalidator "
                             "(SMARAGD_REVAL_WORKERS=0); track bounce is a no-op" );
        return;
    }

    std::shared_ptr<twComponent> root = track_->getRootComponent();
    if( !root ) return;

    // Beside the sidecar store root (a SIBLING "bounce" directory, never
    // inside it — proposal 40 M1b AC 2), honoring the same relocate/off
    // knob the store already resolved.
    const fs::path storeRoot = twSidecarStore::instance().rootPath();
    const fs::path bounceDir = storeRoot.parent_path() / "bounce";
    std::error_code ec;
    fs::create_directories( bounceDir, ec );
    if( ec ) {
        TW_LOGW( "feelflow", "[bounce] cannot create '%s' (%s); track bounce"
                             " is a no-op", bounceDir.string().c_str(),
                 ec.message().c_str() );
        return;
    }

    // One file per track, keyed by the track's own INDEX PATH in the mixer
    // tree (never its address — that is not stable across runs, and it is
    // what would have made the bounce file undiscoverable from a script: a
    // qxa case has no way to spell a pointer). A bounce for track X always
    // overwrites the SAME path, so repeated activation (a re-bounce after
    // an edit) never leaks files; a track that MOVES gets a fresh path,
    // which is a harmless orphan (the same class the media cache's Cleanup…
    // dialog already exists for) rather than a wrong answer.
    SObject *mixerRoot = splacements::rootContainer( project );
    const QList<int> trackPath = strackpath::pathOf( mixerRoot, track_ );
    QString pathStr = trackPath.isEmpty() ? QStringLiteral( "root" )
                                          : QStringLiteral( "" );
    for( int i = 0; i < trackPath.size(); ++i ) {
        if( i > 0 ) pathStr += QLatin1Char( '-' );
        pathStr += QString::number( trackPath[i] );
    }
    const fs::path outPath =
        bounceDir / ( "track_" + pathStr.toStdString() + ".wav" );
    bouncePath_ = outPath.string();

    tw303aEnvironment *env = SAppContext::get().get303aEnvironment();
    const std::uint32_t sampleRate = env ? (std::uint32_t) env->getSRate() : 48000;

    audio::RenderParams params;
    params.outputPath  = bouncePath_;
    params.format       = audio::AudioFormat::WAV;
    params.extent        = audio::RenderParams::Extent::EntireProject;
    // 0 = "ask the graph": the TRACK's own declared width (project channels,
    // proposal 36 B4 — a track has no bus count of its own), never a second
    // number that could drift from it.
    params.channels     = 0;
    params.startTimeSec = 0.0;
    params.endTimeSec   = project->getDurationSeconds();

    // Staleness snapshot, taken at the START of the render (proposal 40
    // section 4.3 / AC 4 — the SCut::contentEpochForCapture_ pattern).
    const std::uint64_t epochAtStart = root->contentEpochNow();

    // Proposal 40 M3: the EFFECTIVE analysis params are built HERE, on the
    // calling (main) thread, from the track's mode/trained state at this
    // exact moment -- never re-read from the track inside the background
    // job, which runs arbitrarily later on another thread and would then be
    // reading STrack state with no synchronization of its own. Captured by
    // value into the closures below, exactly like epochAtStart already is.
    const twGrooveAnalysisParams gp = buildEffectiveGrooveParams( track_ );
    std::vector<uint8_t> gpBlob;
    gp.serialize( gpBlob );
    const uint64_t gpHash =
        twSidecarStore::hashParams( gpBlob.data(), gpBlob.size() );

    // Always recreate, mirroring SApplication::startRender's own comment
    // ("Always recreate for reproducibility") — and it lets ~SFeelFlowTrackBounce
    // join a STALE session cleanly even if start() is called again mid-flight
    // by a future caller (today's callers all check isBouncing() first).
    session_ = std::make_unique<audio::RenderSession>();
    session_->setScheduler( reval );

    // AC 3: per-chain pruning over the track's OWN chain components, never
    // the export path's global walk. The four named in the design (trackMix,
    // pluginChain, gainStage, rewire) are NOT the whole story, measured: a
    // twPluginChain forwards its cache to its LAST INSERT (CLAUDE.md's own
    // metering note) rather than holding pages itself, so an inserted
    // plugin's own twPluginInsert is a FIFTH cache the four-component scope
    // would silently miss — measured leaking the fixture's full page count
    // (12/12, i.e. un-pruned) the first time a plugin sat in the chain
    // during a re-bounce. Every slot's insert joins the scope for exactly
    // that reason.
    std::vector<std::weak_ptr<twComponent>> pruneScope;
    if( auto tm = track_->trackMixComponent() )    pruneScope.push_back( tm );
    if( auto pc = track_->pluginChainComponent() ) pruneScope.push_back( pc );
    if( auto gs = track_->gainStageComponent() )   pruneScope.push_back( gs );
    pruneScope.push_back( root );
    if( SPluginChain *chain = track_->getPluginChain() ) {
        for( int i = 0; i < chain->getSlotCount(); ++i ) {
            if( SPluginSlot *slot = chain->getSlotAt( i ) ) {
                // peekInsert(), never getInsert(): a bounce must not
                // MATERIALIZE a processor for a slot that has none yet.
                if( auto insert = slot->peekInsert() )
                    pruneScope.push_back( insert );
            }
        }
    }
    session_->setPruneScope( std::move( pruneScope ) );

    if( !bouncing_ ) bouncing_ = std::make_shared<std::atomic<bool>>( false );
    bouncing_->store( true, std::memory_order_release );

    // The closure OWNS everything it touches beyond raw pointers that outlive
    // it by construction (analysis-lane lifetime rule, SPlainWave::
    // enqueueGrooveAnalysis's own discipline): the badge flag by shared_ptr,
    // `this`/`project`/`reval` as raw pointers valid for the reasons the
    // destructor's comment above and enqueueGrooveAnalysis's own precedent
    // both give.
    SFeelFlowTrackBounce *self = this;
    std::shared_ptr<std::atomic<bool>> bouncingFlag = bouncing_;
    const std::string bouncePathCopy = bouncePath_;

    session_->onComplete = [self, project, reval, epochAtStart, bouncingFlag,
                            bouncePathCopy, gp, gpBlob, gpHash](
                               bool success, const char *error ) {
        if( !success ) {
            TW_LOGW( "feelflow", "[bounce] render failed: %s",
                     error ? error : "(no error message)" );
            bouncingFlag->store( false, std::memory_order_release );
            return;
        }

        // Proposal 40 M1b AC 5: the SAME analysis lane
        // SPlainWave::enqueueGrooveAnalysis() uses, so a concurrent RENDER's
        // pauseBackground() quiesces this too. Runs later, off THIS thread.
        reval->scheduleAnalysisJob(
            [self, project, epochAtStart, bouncingFlag, bouncePathCopy,
             gp, gpBlob, gpHash]() {
                tw303aEnvironment *jobEnv =
                    SAppContext::get().get303aEnvironment();
                if( jobEnv ) {
                    // Bytes -> file -> decode, the recording path's
                    // precedent (proposal 21 D6/D7): no in-memory wave ctor
                    // exists, so the bounce is read back the same way any
                    // decoded sample is. A bare twSampleSource, NEVER an
                    // SPlainWave -- the bounce must not appear in the
                    // project's extern-file list.
                    twSampleSource src( *jobEnv,
                                        QString::fromStdString( bouncePathCopy ) );
                    if( src.wasLoaded() ) {
                        const twContentHash content = src.contentHash();
                        if( !content.isNull() ) {
                            // gp/gpBlob/gpHash: the EFFECTIVE params (mode +
                            // trained structure) built on the MAIN thread by
                            // start(), captured by value -- M1b's M1 default
                            // path, extended proposal 40 M3's way (never
                            // re-read from the track on this thread).

                            // Skip the heavy pass when both aspects for THIS
                            // bounce's exact content already validate (M1's
                            // own skip rule in enqueueGrooveAnalysis) — a
                            // re-bounce whose audio came out byte-identical
                            // (an edit that changed the epoch but not the
                            // rendered content) still epoch-refreshes below.
                            const bool haveRes = twSidecarStore::instance().load(
                                content, twAspect::GrooveRes,
                                twAspect::GrooveResVersion, gpHash ) != nullptr;
                            const bool haveEv = twSidecarStore::instance().load(
                                content, twAspect::GrooveEv,
                                twAspect::GrooveEvVersion, gpHash ) != nullptr;
                            // M3c: all THREE aspects (see
                            // enqueueGrooveAnalysis's own comment).
                            const bool haveDyn = twSidecarStore::instance().load(
                                content, twAspect::GrooveDyn,
                                twAspect::GrooveDynVersion, gpHash ) != nullptr;

                            if( haveRes && haveEv && haveDyn ) {
                                // Already valid for this exact bounce
                                // content -- skip the heavy pass, but this
                                // IS a fresh, successful bounce: refresh the
                                // epoch snapshot so isStale() reflects it.
                                // The content hash write (M2) is BEFORE
                                // haveResult_'s release store for the same
                                // reason epochAtBounce_'s is: haveResult_ is
                                // what publishes both to feelFlowForUi().
                                self->contentHashLo_.store(
                                    content.lo, std::memory_order_relaxed );
                                self->contentHashHi_.store(
                                    content.hi, std::memory_order_relaxed );
                                self->paramsHashAtBounce_.store(
                                    gpHash, std::memory_order_relaxed );
                                // physUnitNames_/phys*_ are DELIBERATELY left
                                // untouched here -- see their own doc
                                // (sfeelflowbounce.h) for why that is correct
                                // on this branch.
                                self->epochAtBounce_.store(
                                    epochAtStart, std::memory_order_release );
                                self->haveResult_.store(
                                    true, std::memory_order_release );
                            } else {
                                const uint32_t nCh = (uint32_t) src.channels();
                                const uint64_t n    = (uint64_t) src.length();
                                const uint32_t rate = (uint32_t) src.sampleRate();
                                std::vector<const float *> chans( nCh );
                                for( uint32_t c = 0; c < nCh; c++ )
                                    chans[c] = src.channelData( (idx_t) c );

                                const twGrooveAspectPayloads built =
                                    twGrooveBuildAspectPayloads(
                                        chans.data(), nCh, n, rate, gp );
                                // Honest-empty, mirroring enqueueGrooveAnalysis:
                                // an unanalyzable bounce (no recoverable
                                // tatum) stores nothing.
                                if( built.nUnits > 0 ) {
                                    twQafInfo qi;
                                    qi.contentHash  = content;
                                    qi.sourceRate   = rate;
                                    qi.channels     = nCh;
                                    qi.sourceFrames = n;
                                    qi.params       = gpBlob;

                                    qi.aspectId      = twAspect::GrooveRes;
                                    qi.aspectVersion = twAspect::GrooveResVersion;
                                    qi.recordStride  =
                                        (uint64_t)( built.nUnits + 1 ) * 4;
                                    qi.recordCount   = built.resRecordCount;
                                    qi.hopFrames     = built.hopFrames;
                                    twSidecarStore::instance().store(
                                        qi, built.resPayload.data(),
                                        (uint64_t) built.resPayload.size() );

                                    qi.aspectId      = twAspect::GrooveEv;
                                    qi.aspectVersion = twAspect::GrooveEvVersion;
                                    qi.recordStride  = 20;
                                    qi.recordCount   = built.evRecordCount;
                                    qi.hopFrames     = 0;
                                    twSidecarStore::instance().store(
                                        qi, built.evPayload.data(),
                                        (uint64_t) built.evPayload.size() );

                                    // Proposal 40 M3c: the pass-1
                                    // dynamics (Tier B).
                                    qi.aspectId      = twAspect::GrooveDyn;
                                    qi.aspectVersion = twAspect::GrooveDynVersion;
                                    // v2 (M3e): 6 float32 per unit --
                                    // support/tension/cosPhi/sinPhi/slip/
                                    // dissip (twaspects.h).
                                    qi.recordStride  =
                                        (uint64_t) built.nUnits * 8 * 4;
                                    qi.recordCount   = built.dynRecordCount;
                                    qi.hopFrames     = built.hopFrames;
                                    twSidecarStore::instance().store(
                                        qi, built.dynPayload.data(),
                                        (uint64_t) built.dynPayload.size() );

                                    // ---- proposal 44 C5: "body.pose" ----
                                    // THE PLANT RUNS HERE, on the ANALYSIS
                                    // side, because this is where the units'
                                    // COUPLINGS are. The read side has no `k`
                                    // and must not: recovering the music from
                                    // the drive needs it divided out
                                    // (tw/body CONTRACT 31), and a reader that
                                    // had to reconstruct it would be a second
                                    // implementation of a decision that
                                    // belongs to one.
                                    //
                                    // ITS PARAMS BLOB NESTS THE GROOVE ONE
                                    // rather than extending it -- gpBlob is
                                    // untouched, so a body change re-keys this
                                    // aspect and NOTHING ELSE (twaspects.h's
                                    // "body.pose" entry has the argument).
                                    {
                                        twBodyPlantInput in;
                                        in.nUnits = built.nUnits;
                                        in.dyn = twGrooveDecodeDynPayload(
                                            built.dynPayload.data(),
                                            (uint64_t) built.dynPayload.size(),
                                            built.nUnits );
                                        for( const twGrooveCounterTension &ct
                                             : built.counterTension ) {
                                            in.unitNames.push_back( ct.name );
                                            in.unitK.push_back( ct.k );
                                        }
                                        in.dtSec = built.hopFrames && rate
                                                 ? (double) built.hopFrames
                                                   / (double) rate
                                                 : 0.01;
                                        // M and H are the ONE body input and
                                        // are still defaults: proposal 44
                                        // section 8 question 2 (global or
                                        // per-project) is unanswered, so
                                        // nothing writes them yet. When it
                                        // does, only this aspect re-keys.
                                        twBodyPoseParams bp;
                                        bp.groove = gp;
                                        std::vector<uint8_t> bpBlob;
                                        bp.serialize( bpBlob );
                                        std::vector<twBodyPoseRecord> poseRecs =
                                            twBodyPlantRun( in, bp.body );
                                        if( !poseRecs.empty() ) {
                                            std::vector<uint8_t> posePayload;
                                            twBodyPoseEncode( poseRecs, posePayload );
                                            twQafInfo pi = qi;
                                            pi.params        = bpBlob;
                                            pi.aspectId      = twAspect::BodyPose;
                                            pi.aspectVersion = twAspect::BodyPoseVersion;
                                            pi.recordStride  =
                                                (uint64_t) twBodyPoseDof::Count * 3 * 4;
                                            pi.recordCount   = poseRecs.size();
                                            pi.hopFrames     = built.hopFrames;
                                            twSidecarStore::instance().store(
                                                pi, posePayload.data(),
                                                (uint64_t) posePayload.size() );
                                        }
                                    }

                                    // Proposal 40 M3: the physical-readout
                                    // summary, IN-MEMORY only (never part of
                                    // either QAF payload above) -- written
                                    // strictly before haveResult_'s release
                                    // store below, per the members' own doc.
                                    self->physUnitNames_.clear();
                                    self->physMeanSinDeltaPhi_.clear();
                                    self->physVarSinDeltaPhi_.clear();
                                    self->physMeanF_.clear();
                                    for( const twGrooveCounterTension &ct
                                         : built.counterTension ) {
                                        self->physUnitNames_.push_back( ct.name );
                                        self->physMeanSinDeltaPhi_.push_back(
                                            (float) ct.meanSinDeltaPhi );
                                        self->physVarSinDeltaPhi_.push_back(
                                            (float) ct.varSinDeltaPhi );
                                        self->physMeanF_.push_back(
                                            (float) ct.meanF );
                                    }

                                    self->contentHashLo_.store(
                                        content.lo, std::memory_order_relaxed );
                                    self->contentHashHi_.store(
                                        content.hi, std::memory_order_relaxed );
                                    self->paramsHashAtBounce_.store(
                                        gpHash, std::memory_order_relaxed );
                                    self->epochAtBounce_.store(
                                        epochAtStart, std::memory_order_release );
                                    self->haveResult_.store(
                                        true, std::memory_order_release );
                                }
                            }
                        }
                    } else {
                        TW_LOGW( "feelflow", "[bounce] failed to decode '%s'",
                                 bouncePathCopy.c_str() );
                    }
                }

                // Proposal 40 M2: force exactly one UI-cache reload on the
                // next feelFlowForUi() call -- the onsetsForUi() completion
                // discipline verbatim (splainwave.cpp). Unconditional: even
                // a failed/unanalyzable bounce may have left a STALE cached
                // result behind (this holder's previous, now-superseded
                // success), and a decode failure must not leave the old
                // content's compliance data on screen.
                std::atomic_store( &self->uiCache_->ptr,
                                   std::shared_ptr<const SFeelFlowUiData>() );

                bouncingFlag->store( false, std::memory_order_release );
                QMetaObject::invokeMethod( project, "notifyCaptureRevalidated",
                                           Qt::QueuedConnection );
            } );
    };

    if( !session_->start( root, params, sampleRate ) ) {
        TW_LOGW( "feelflow", "[bounce] failed to start: %s",
                 session_->errorMessage() );
        bouncing_->store( false, std::memory_order_release );
    }
}

std::shared_ptr<const SFeelFlowUiData> SFeelFlowTrackBounce::feelFlowForUi() const
{
    if( !uiCache_ ) return nullptr;                 // pre-ctor defensive
    if( auto cached = std::atomic_load( &uiCache_->ptr ) )
        return cached;                              // hit (possibly empty)

    // First call (or post-analysis invalidation): read the "groove.res"
    // sidecar ONCE. A miss (never bounced, decode failure, or the payload's
    // geometry does not check out) caches an EMPTY result so a repaint never
    // re-hits the store.
    auto fresh = std::make_shared<SFeelFlowUiData>();
    if( haveResult_.load( std::memory_order_acquire ) ) {
        twContentHash content;
        content.lo = contentHashLo_.load( std::memory_order_relaxed );
        content.hi = contentHashHi_.load( std::memory_order_relaxed );
        if( !content.isNull() ) {
            // Params-AGNOSTIC (loadAny), mirroring SPlainWave::onsetsForUi():
            // this is a UI reader, not the job that chose the analysis
            // params, and M1/M1b ship no per-clip/per-track tuning yet.
            std::unique_ptr<twQafReader> reader =
                twSidecarStore::instance().loadAny(
                    content, twAspect::GrooveRes, twAspect::GrooveResVersion );
            if( reader && reader->info().recordStride >= 4
                && reader->info().recordCount > 0 ) {
                // nUnits is not in the QAF header (twaspects.h's "groove.res"
                // doc) -- derive it from this file's own geometry, the same
                // computation assert-groove-aspect makes.
                const uint32_t nUnits =
                    (uint32_t)( reader->info().recordStride / 4 - 1 );
                std::vector<uint8_t> payload;
                if( reader->readAllPayload( payload ) ) {
                    const std::vector<twGrooveResRecord> decoded =
                        twGrooveDecodeResPayload( payload.data(), payload.size(),
                                                  nUnits );
                    fresh->compliance.reserve( decoded.size() );
                    // Proposal 40 M3: kept alongside compliance (same decode,
                    // same records) for the panel's per-pendulum energy bars
                    // -- the ONLY new thing this read path does is stop
                    // discarding the unitPower columns M2 already decoded.
                    fresh->nUnits = nUnits;
                    // Metric ids must be STABLE across warm and cold runs (a
                    // qxa script addresses "power:reference"), but
                    // physUnitNames_ is in-memory job state that stays EMPTY
                    // when the first bounce of a process hits a warm store
                    // (the documented M3 gap). Fall back to the default
                    // ensemble's own names when the column count matches it
                    // -- every current caller analyzes with exactly that
                    // ensemble; a future custom ensemble of a different size
                    // leaves the list EMPTY, so derive falls to its own
                    // unit<i> naming and the pose leaves every part at 0
                    // rather than guessing which unit is somebody's pelvis.
                    fresh->unitNames = physUnitNames_;
                    if( fresh->unitNames.empty() ) {
                        const std::vector<twGroovePendulumUnitSpec> def =
                            twGrooveDefaultEnsemble();
                        if( def.size() == nUnits )
                            for( const twGroovePendulumUnitSpec &u : def )
                                fresh->unitNames.push_back( u.name );
                    }
                    fresh->perUnitPower.reserve( decoded.size() * (size_t) nUnits );
                    for( const twGrooveResRecord &rec : decoded ) {
                        fresh->compliance.push_back( rec.compliance );
                        for( uint32_t u = 0; u < nUnits; u++ )
                            fresh->perUnitPower.push_back(
                                u < rec.unitPower.size() ? rec.unitPower[u] : 0.0f );
                    }
                    if( !decoded.empty() )
                        fresh->hopFrames = (uint32_t) reader->info().hopFrames;

                    // Proposal 40 M3b: the metric lab. One more loadAny on
                    // the SAME cached reload -- "groove.ev" was written by
                    // the same job that wrote "groove.res", so a res hit
                    // with an ev miss is legitimate only for a pre-M1 store
                    // entry and derives the res-only series with an empty
                    // event list (every event-derived hop holds the
                    // sentinel). Derived HERE, once per reload, so the
                    // paint path and the panel read one immutable snapshot.
                    if( fresh->hopFrames != 0 && reader->info().sourceRate != 0 ) {
                        std::vector<twGrooveEvRecord> evRecords;
                        std::unique_ptr<twQafReader> evReader =
                            twSidecarStore::instance().loadAny(
                                content, twAspect::GrooveEv,
                                twAspect::GrooveEvVersion );
                        if( evReader ) {
                            std::vector<uint8_t> evPayload;
                            if( evReader->readAllPayload( evPayload ) )
                                evRecords = twGrooveDecodeEvPayload(
                                    evPayload.data(), evPayload.size() );
                        }
                        // M3c: the dynamics aspect. A miss (a pre-M3c
                        // store entry) derives the Tier A series only --
                        // the Tier B rows are ABSENT, never ghosts.
                        std::vector<twGrooveDynRecord> dynRecords;
                        std::unique_ptr<twQafReader> dynReader =
                            twSidecarStore::instance().loadAny(
                                content, twAspect::GrooveDyn,
                                twAspect::GrooveDynVersion );
                        if( dynReader ) {
                            std::vector<uint8_t> dynPayload;
                            if( dynReader->readAllPayload( dynPayload ) )
                                dynRecords = twGrooveDecodeDynPayload(
                                    dynPayload.data(), dynPayload.size(),
                                    nUnits );
                        }
                        // Metric ids must be STABLE across warm and cold
                        // runs (a qxa script addresses "power:reference"),
                        // but physUnitNames_ is in-memory job state that
                        // stays EMPTY when the first bounce of a process
                        // hits a warm store (the documented M3 gap). Fall
                        // back to the default ensemble's own names when
                        // the column count matches it -- every current
                        // caller analyzes with exactly that ensemble; a
                        // future custom ensemble of a different size drops
                        // to derive's own unit<i> naming rather than
                        // guessing.
                        //
                        // M3e: the fallback is applied to `fresh->unitNames`
                        // ITSELF (below, where it used to be a bare copy of
                        // physUnitNames_) rather than to a local, so the
                        // PUPPET POSE -- which maps ensemble units onto body
                        // parts BY NAME out of this very snapshot -- resolves
                        // exactly the units the metric ids are built from.
                        // With the fallback local, a warm-store first bounce
                        // gave the pose an EMPTY name list and every part sat
                        // at 0 while the metric rows read fine: a puppet that
                        // stood still on the second run of the same case.
                        const std::vector<std::string> &unitNames =
                            fresh->unitNames;
                        // Read-side constants: the section 2.3 literature
                        // defaults. Per-user tuning of these is M5's
                        // Options page, deliberately not here.
                        fresh->metrics = twGrooveDeriveMetrics(
                            decoded, evRecords, dynRecords, fresh->hopFrames,
                            reader->info().sourceRate, unitNames,
                            twGrooveReadParams{} );
                        // M3e: keep the records rather than discarding
                        // them, so the puppet pose is a pure function of
                        // this snapshot. Moved AFTER the derive call --
                        // twGrooveDeriveMetrics takes them by const ref.
                        fresh->dyn = std::move( dynRecords );

                        // Proposal 44 C5: "body.pose". A MISS is the normal
                        // case for any content analysed before this aspect
                        // existed, and it is not a failure -- sFeelFlowPoseAt
                        // falls back to the M3e direct mapping and every
                        // number is byte-identical to C3's (AC5.4). The
                        // params blob NESTS the groove one, so this loadAny
                        // deliberately does not reuse gpHash.
                        {
                            // THE EFFECTIVE groove params, not the defaults.
                            // The write side keys on buildEffectiveGrooveParams
                            // (which is Trained mode for a track carrying a
                            // frozen structure), so looking up under a
                            // default-constructed one would MISS on exactly
                            // those tracks -- silently, and only for them.
                            twBodyPoseParams bp;
                            bp.groove = buildEffectiveGrooveParams( track_ );
                            std::vector<uint8_t> bpBlob;
                            bp.serialize( bpBlob );
                            std::unique_ptr<twQafReader> poseReader =
                                twSidecarStore::instance().load(
                                    content, twAspect::BodyPose,
                                    twAspect::BodyPoseVersion,
                                    twSidecarStore::hashParams(
                                        bpBlob.data(), bpBlob.size() ) );
                            if( poseReader ) {
                                std::vector<uint8_t> posePayload;
                                if( poseReader->readAllPayload( posePayload ) )
                                    fresh->pose = twBodyPoseDecode(
                                        posePayload.data(), posePayload.size() );
                            }

                            // A MISS ON A WARM STORE REGENERATES THE POSE HERE,
                            // and leaving that out was a real defect, found the
                            // moment C8 bumped BodyPoseVersion.
                            //
                            // The plant runs on the ANALYSIS side, and the
                            // analysis is exactly what a warm store SKIPS. So a
                            // store holding valid groove aspects but no
                            // body.pose -- which is every store written before
                            // C5, and every store at all after any body.pose
                            // version bump -- never regenerated one: the puppet
                            // fell back to the M3e direct mapping silently and
                            // permanently, and only a cleared cache would ever
                            // have shown otherwise. Measured: the qxa cases
                            // passed cold and failed warm.
                            //
                            // WHY THE READ SIDE CAN DO THIS AT ALL, given that
                            // tw/body CONTRACT 31 says recovering the music
                            // needs `k` divided out and `k` is not in the wire
                            // format: `k` is a property of the ENSEMBLE SPEC,
                            // not of the audio, and `bp.groove` right above IS
                            // that spec. So this is the same k the analysis
                            // used, read from the same place, rather than a
                            // second derivation of it. Units are matched BY
                            // NAME, never by index, because unitNames may have
                            // come from the default-ensemble fallback.
                            if( fresh->pose.empty() && !fresh->dyn.empty()
                                && nUnits > 0 && !fresh->unitNames.empty() ) {
                                twBodyPlantInput bin;
                                bin.nUnits    = nUnits;
                                bin.unitNames = fresh->unitNames;
                                bin.dyn       = fresh->dyn;
                                bin.dtSec     = fresh->hopFrames
                                                && reader->info().sourceRate
                                              ? (double) fresh->hopFrames
                                                / (double) reader->info().sourceRate
                                              : 0.01;
                                for( const std::string &un : fresh->unitNames ) {
                                    double k = 0.0;
                                    for( const twGroovePendulumUnitSpec &us
                                         : bp.groove.pendulum.ensemble )
                                        if( us.name == un ) { k = us.k; break; }
                                    bin.unitK.push_back( k );
                                }
                                std::vector<twBodyPoseRecord> recs =
                                    twBodyPlantRun( bin, bp.body );
                                if( !recs.empty() ) {
                                    fresh->pose = recs;
                                    std::vector<uint8_t> payload;
                                    twBodyPoseEncode( recs, payload );
                                    twQafInfo pi   = reader->info();
                                    pi.params        = bpBlob;
                                    pi.aspectId      = twAspect::BodyPose;
                                    pi.aspectVersion = twAspect::BodyPoseVersion;
                                    pi.recordStride  =
                                        (uint64_t) twBodyPoseDof::Count * 3 * 4;
                                    pi.recordCount   = recs.size();
                                    pi.hopFrames     = fresh->hopFrames;
                                    twSidecarStore::instance().store(
                                        pi, payload.data(),
                                        (uint64_t) payload.size() );
                                }
                            }
                        }
                    }
                }
            }
        }

        // Proposal 40 M3: the pass-2 physical-readout summary -- IN-MEMORY
        // only (see the members' own doc for when this is, and is not,
        // fresh). Copied unconditionally alongside the disk read above so a
        // process that has bounced at least once always shows what it has,
        // even on a "skip the heavy pass" re-bounce that left these members
        // exactly as a prior real pass set them.
        // unitNames is resolved ABOVE, next to nUnits, so the default-ensemble
        // fallback reaches the metric ids AND the puppet pose from one list.
        // This is the "the sidecar read missed entirely" path.
        if( fresh->unitNames.empty() ) fresh->unitNames = physUnitNames_;
        fresh->meanSinDeltaPhi = physMeanSinDeltaPhi_;
        fresh->varSinDeltaPhi  = physVarSinDeltaPhi_;
        fresh->meanF           = physMeanF_;
    }

    auto published = std::shared_ptr<const SFeelFlowUiData>( std::move( fresh ) );
    std::atomic_store( &uiCache_->ptr, published );
    return published;
}
