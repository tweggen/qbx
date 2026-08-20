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
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sources/twsamplesource.h"

#include <QMetaObject>
#include <QString>

#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

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
    return root->contentEpochNow() != epochAtBounce_.load( std::memory_order_acquire );
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
                            bouncePathCopy]( bool success, const char *error ) {
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
            [self, project, epochAtStart, bouncingFlag, bouncePathCopy]() {
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
                            // M1's default analysis-side params, verbatim --
                            // M1b points the SAME encoder at a different
                            // signal (the bounce) without touching M1's
                            // per-clip path (proposal 40 section 4.3).
                            twGrooveAnalysisParams gp;
                            std::vector<uint8_t> gpBlob;
                            gp.serialize( gpBlob );
                            const uint64_t gpHash = twSidecarStore::hashParams(
                                gpBlob.data(), gpBlob.size() );

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

                            if( haveRes && haveEv ) {
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

                                    self->contentHashLo_.store(
                                        content.lo, std::memory_order_relaxed );
                                    self->contentHashHi_.store(
                                        content.hi, std::memory_order_relaxed );
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
                    for( const twGrooveResRecord &rec : decoded )
                        fresh->compliance.push_back( rec.compliance );
                    if( !decoded.empty() )
                        fresh->hopFrames = (uint32_t) reader->info().hopFrames;
                }
            }
        }
    }

    auto published = std::shared_ptr<const SFeelFlowUiData>( std::move( fresh ) );
    std::atomic_store( &uiCache_->ptr, published );
    return published;
}
