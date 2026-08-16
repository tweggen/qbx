
#include <stdio.h>
#include <cstring>

#include <QDir>
#include <QFileInfo>
#include <qobject.h>
#include <qwidget.h>
#include <qlabel.h>
#include "qmessagebox.h"

#include "app/model/sappcontext.h"
#include "app/model/sfilepathref.h"
#include "app/model/sproject.h"
#include "tw/schedule/capture_aspects.h"  // Preview/Playback/... bits
#include "tw/schedule/capture_revalidator.h"
#include "tw/graph/twcomponent.h"
#include "tw/sidecar/twanalyzers.h"
#include "tw/sidecar/twaspects.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sources/twrandomsource.h"
#include "tw/sources/twsamplesource.h"
#include "tw/sources/twwavinput.h"
#include "app/objects/wave/splainwave.h"
#include "app/objects/wave/splainwaverndrinline.h"
#include "app/persistence/sprojectloader.h"

int SPlainWave::serializeSelfAttributes( QTextStream &o )
{
    // The reference is stored PORTABLY — relative to the project file, or to
    // "~", or absolute only as a last resort. See sfilepathref.h for which of
    // the three applies when. The in-memory fileName_ stays absolute; only the
    // on-disk spelling changes.
    SProject *project = qobject_cast<SProject *>( parent() );
    const QString stored = SFilePathRef::toStored(
        getFileName(), project ? project->projectFilePath() : QString() );
    o << " filename='" << stored << "'";
    SExternFile::serializeSelfAttributes( o );
    return 0;
}

bool SPlainWave::hasPreview() const
{
    return true;
}

int SPlainWave::getPreview( preview_t *dest,
                             offset_t start, length_t length,
                             offset_t nProbes )
{
    if( !cpWave_ ) return -1;

    // A wave previews from its own PCM, through the shared straight-preview
    // path (sidecar-backed, see fetchPreviewSidecar below).
    //
    // WHAT USED TO BE HERE, and why it is gone (proposal 36 trap 26, settled by
    // B8). This function first asked getCapture(Preview) for an aspect page and,
    // when one existed, reinterpret_cast<const preview_t*> its 256 kB buffer and
    // memcpy'd the head of it into `dest`. That was a TYPE CONFUSION: the only
    // writer of a Preview aspect page, CaptureRevalidator::dispatchRecomputation,
    // memcpys FLOAT SAMPLES there (freezePreviewPage at ~1 kHz, channel 0) --
    // 4-byte IEEE-754, not 2-byte {int8 min, int8 max} probes. Read the wrong
    // way, one full-scale sample (1.0f = 0x3F800000 LE) becomes the two probes
    // {0,0} and {-128,63}: a comb of fixed-height ticks, not a waveform.
    //
    // It was never reached. An SObject's currentPage_ is written only by
    // swapPages_nolock(), i.e. only from CaptureRevalidator::processRevalidationJob,
    // i.e. only for an object handed to scheduleRevalidation() -- whose only two
    // call sites in the tree are SCut members passing `this`. An SPlainWave is
    // not an SCut, so getCapture() here could only ever return nullptr and this
    // fell through to getStraightPreview() on every call. Deleting it is
    // therefore behaviour-preserving, and preferable to "fixing" the cast:
    // a CapturePageData carries no probe count, hop or duration, so even with
    // the right element type it could not answer a (start, length, nProbes)
    // question -- it ignored all three and copied the head of the buffer.
    //
    // A Preview aspect page IS: float samples of the object's output, decimated
    // to ~1 kHz, channel 0, with no geometry attached. Its only consumer,
    // SCut::getPreview, uses the page's EXISTENCE as a readiness signal and
    // never reads data. See tw/pages/capture_page_pool.h.
    return getStraightPreview( dest, start, length, nProbes );
}


namespace {

// Canonical preview.peaks params blob: { uint32 projectRate }, little-endian
// (tw/sidecar/twaspects.h). The preview is computed over the project-rate
// view, so its bytes are rate-dependent; the rate is therefore key material.
void previewParams( uint32_t rate, std::vector<uint8_t> &out )
{
    out.resize( 4 );
    out[0] = (uint8_t)( rate & 0xff );
    out[1] = (uint8_t)( ( rate >> 8 ) & 0xff );
    out[2] = (uint8_t)( ( rate >> 16 ) & 0xff );
    out[3] = (uint8_t)( ( rate >> 24 ) & 0xff );
}

} // namespace

bool SPlainWave::fetchPreviewSidecar( preview_t *dest, offset_t nProbes,
                                      offset_t skip, offset_t forLength )
{
    if( !cpWave_ ) return false;
    twContentHash content = cpWave_->contentHash();
    twRandomSource *src = cpWave_->getSource();
    if( content.isNull() || !src ) return false;
    std::vector<uint8_t> params;
    previewParams( (uint32_t) src->sampleRate(), params );
    auto reader = twSidecarStore::instance().load(
        content, twAspect::PreviewPeaks, twAspect::PreviewPeaksVersion,
        twSidecarStore::hashParams( params.data(), params.size() ) );
    if( !reader ) return false;
    // Adoption cross-check: only exact geometry is a hit — anything else
    // (duration drift, changed probe layout) recomputes and re-stores.
    const twQafInfo &qi = reader->info();
    if( qi.recordStride  != sizeof( preview_t )
        || qi.recordCount  != (uint64_t) nProbes
        || qi.hopFrames    != (uint64_t) skip
        || qi.sourceFrames != (uint64_t) forLength
        // WIDTH is part of the geometry since B8: the probe envelope folds
        // every channel, so a preview computed over a different channel count
        // is a different preview. The version bump orphans v1 files; this
        // catches a v2 file whose source has since changed width.
        || qi.channels     != (uint32_t) src->channels() ) return false;
    return reader->readRecords( dest, 0, (uint64_t) nProbes );
}

void SPlainWave::storePreviewSidecar( const preview_t *data, offset_t nProbes,
                                      offset_t skip, offset_t forLength )
{
    if( !cpWave_ ) return;
    twContentHash content = cpWave_->contentHash();
    twRandomSource *src = cpWave_->getSource();
    if( content.isNull() || !src ) return;
    twQafInfo qi;
    qi.aspectId      = twAspect::PreviewPeaks;
    qi.aspectVersion = twAspect::PreviewPeaksVersion;
    qi.contentHash   = content;
    // For this aspect all geometry is expressed at the PROJECT rate (the rate
    // the preview was computed at) — see twaspects.h.
    qi.sourceRate    = (uint32_t) src->sampleRate();
    // The SOURCE's width. The payload is one envelope folded over all of them
    // (proposal 36 B8) — it was a hard-coded 1 while the fold was channel 0.
    qi.channels      = (uint32_t) src->channels();
    qi.sourceFrames  = (uint64_t) forLength;
    qi.recordStride  = sizeof( preview_t );
    qi.recordCount   = (uint64_t) nProbes;
    qi.hopFrames     = (uint64_t) skip;
    previewParams( qi.sourceRate, qi.params );
    twSidecarStore::instance().store( qi, data,
                                      (uint64_t) nProbes * sizeof( preview_t ) );
}

QString SPlainWave::getFileName() const
{
    return fileName_;
}

std::shared_ptr<twComponent> SPlainWave::getRootComponent()
{
    return cpWave_;
}

twRandomSource *SPlainWave::getRandomSource()
{
    return cpWave_ ? cpWave_->getSource() : NULL;
}

QWidget *SPlainWave::getDetailEditWidget( QWidget *parent )
{
    // FIXME: Reset pointer on destroy.
    return new QLabel( "plainWave: Nothing to edit now.", parent );
}

QWidget *SPlainWave::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *SPlainWave::getInlineRenderer()
{
    if( !inlineRenderer_ ) {
        inlineRenderer_ = new SPlainWaveRendererInline( *this );
    }
    return inlineRenderer_;
}

bool SPlainWave::hasDuration() const
{
    return cpWave_ != NULL;
}

length_t SPlainWave::getDuration() const
{
    if( cpWave_ ) {
        return cpWave_->getLength();
    } else {
        return 0;
    }
}

int SPlainWave::setWave( const QString fileName )
{
    // Fail, if we already have a wave set.
    if( cpWave_ ) return -2;
    fileName_ = fileName;
    cpWave_ = std::make_shared<twWavInput>( *(SAppContext::get().get303aEnvironment()), fileName );
    cpWave_->init();
    if( !cpWave_->wasLoaded() ) {
        // Suppress dialog in headless/test mode; log to stderr instead
        if( SAppContext::get().testOutputDir().isEmpty() ) {
            QMessageBox::information( nullptr, "QBX error", "Unable to load file.", QMessageBox::Ok );
        } else {
            qWarning() << "SPlainWave: unable to load file:" << fileName;
        }
        cpWave_.reset();
        return -1;
    }
    // Add myselves tob the list of extern objects.
    qWarning() << "Filename here is" << fileName_;
    SAppContext::get().getCurrentProject()->addExternObject( *this );
    enqueueAnalysis();
    return 0;
}

void SPlainWave::enqueueAnalysis()
{
    SProject *project = qobject_cast<SProject *>( parent() );
    if( !project ) return;
    CaptureRevalidator *reval = project->getRevalidator();
    if( !reval ) return;                        // SMARAGD_REVAL_WORKERS=0
    if( !twSidecarStore::instance().enabled() ) return;
    twSampleSource *src = cpWave_ ? cpWave_->sampleSource() : nullptr;
    if( !src ) return;
    const twContentHash content = src->contentHash();
    if( content.isNull() ) return;

    // Params are derived from the SOURCE rate (results must be stable across
    // project rates — twaspects.h).
    const uint32_t rate = (uint32_t) src->sampleRate();
    twOnsetParams op;
    op.minSeparationFrames = rate * 3 / 100;    // ~30 ms
    twLoudnessParams lp;
    lp.hopFrames = rate / 100;                  // 10 ms
    lp.winFrames = lp.hopFrames * 2;
    twF0Params fp;
    fp.rate      = rate;
    fp.hopFrames = rate / 100;                  // 10 ms (loudness-aligned)
    fp.winFrames = rate / 30;                   // ~33 ms >= 2 periods of fmin

    std::vector<uint8_t> opBlob, lpBlob, fpBlob;
    op.serialize( opBlob );
    lp.serialize( lpBlob );
    fp.serialize( fpBlob );
    const uint64_t opHash =
        twSidecarStore::hashParams( opBlob.data(), opBlob.size() );
    const uint64_t lpHash =
        twSidecarStore::hashParams( lpBlob.data(), lpBlob.size() );
    const uint64_t fpHash =
        twSidecarStore::hashParams( fpBlob.data(), fpBlob.size() );

    // Skip when all aspects already VALIDATE (version-aware — a bare
    // exists() check would keep files with an outdated aspectVersion forever).
    const bool haveOnsets = twSidecarStore::instance().load(
        content, twAspect::Onsets, twAspect::OnsetsVersion, opHash ) != nullptr;
    const bool haveLoudness = twSidecarStore::instance().load(
        content, twAspect::Loudness, twAspect::LoudnessVersion, lpHash ) != nullptr;
    const bool haveF0 = twSidecarStore::instance().load(
        content, twAspect::F0, twAspect::F0Version, fpHash ) != nullptr;
    if( haveOnsets && haveLoudness && haveF0 ) return;

    if( !analyzing_ )
        analyzing_ = std::make_shared<std::atomic<bool>>( false );
    analyzing_->store( true, std::memory_order_release );

    // The closure OWNS everything it touches (analysis-lane lifetime rule):
    // the wav input (and through it the sample source) via shared_ptr, and
    // the badge flag via shared_ptr. `project` is safe to pass to the queued
    // invokeMethod: the revalidator is owned by SProject and joins its
    // workers before the project dies (the sanctioned revalCompleted bridge).
    std::shared_ptr<twWavInput> wav = cpWave_;
    std::shared_ptr<std::atomic<bool>> flag = analyzing_;
    // Share the UI onset-cache slot so the job can invalidate it on completion
    // (a wave deleted mid-job leaves the closure a valid slot — same lifetime
    // rule as the analyzing_ flag).
    std::shared_ptr<UiOnsetsSlot> uiSlot = uiOnsets_;
    reval->scheduleAnalysisJob(
        [wav, flag, uiSlot, project, content, op, lp, fp, opBlob, lpBlob,
         fpBlob, haveOnsets, haveLoudness, haveF0, rate]() {
            twSampleSource *s = wav->sampleSource();
            if( s ) {
                const uint32_t nCh = (uint32_t) s->channels();
                const uint64_t n   = (uint64_t) s->length();
                std::vector<const float *> chans( nCh );
                for( uint32_t c = 0; c < nCh; c++ )
                    chans[c] = s->channelData( (idx_t) c );

                twQafInfo qi;
                qi.contentHash  = content;
                qi.sourceRate   = rate;
                qi.channels     = nCh;
                qi.sourceFrames = n;

                if( !haveOnsets ) {
                    std::vector<twOnset> onsets =
                        twDetectOnsets( chans.data(), nCh, n, op );
                    // v3 records are PACKED 12-byte {u64 pos, f32 salience}
                    // LE — never the in-memory struct (padding is not a
                    // serialization format).
                    std::vector<uint8_t> payload;
                    payload.reserve( onsets.size() * 12 );
                    for( const twOnset &o : onsets ) {
                        for( int b = 0; b < 8; b++ )
                            payload.push_back( (uint8_t)( o.pos >> ( 8 * b ) ) );
                        uint32_t sb;
                        memcpy( &sb, &o.salience, 4 );
                        for( int b = 0; b < 4; b++ )
                            payload.push_back( (uint8_t)( sb >> ( 8 * b ) ) );
                    }
                    qi.aspectId      = twAspect::Onsets;
                    qi.aspectVersion = twAspect::OnsetsVersion;
                    qi.recordStride  = 12;
                    qi.recordCount   = (uint64_t) onsets.size();
                    qi.hopFrames     = op.hop;
                    qi.params        = opBlob;
                    twSidecarStore::instance().store(
                        qi, payload.data(), (uint64_t) payload.size() );
                }
                if( !haveLoudness ) {
                    std::vector<float> rms =
                        twComputeLoudness( chans.data(), nCh, n, lp );
                    qi.aspectId      = twAspect::Loudness;
                    qi.aspectVersion = twAspect::LoudnessVersion;
                    qi.recordStride  = sizeof( float );
                    qi.recordCount   = (uint64_t) rms.size();
                    qi.hopFrames     = lp.hopFrames;
                    qi.params        = lpBlob;
                    twSidecarStore::instance().store(
                        qi, rms.data(),
                        (uint64_t) rms.size() * sizeof( float ) );
                }
                if( !haveF0 ) {
                    std::vector<float> f0 =
                        twComputeF0( chans.data(), nCh, n, fp );
                    qi.aspectId      = twAspect::F0;
                    qi.aspectVersion = twAspect::F0Version;
                    qi.recordStride  = sizeof( float );
                    qi.recordCount   = (uint64_t) f0.size();
                    qi.hopFrames     = fp.hopFrames;
                    qi.params        = fpBlob;
                    twSidecarStore::instance().store(
                        qi, f0.data(),
                        (uint64_t) f0.size() * sizeof( float ) );
                }
            }
            flag->store( false, std::memory_order_release );
            // Invalidate the UI onset cache so the next paint reloads the
            // freshly written results (null = "not loaded"; onsetsForUi()
            // re-reads the sidecar on the next call).
            if( uiSlot )
                std::atomic_store( &uiSlot->ptr,
                                   std::shared_ptr<const UiOnsets>() );
            // Queued to the UI thread: badge repaint via the existing
            // captureRevalidated() -> update() connection.
            QMetaObject::invokeMethod( project, "notifyCaptureRevalidated",
                                       Qt::QueuedConnection );
        } );
}

SPlainWave::~SPlainWave()
{
    if( cpWave_ ) {
        // Deregister from our OWN project (our QObject parent). Using the app's
        // "current" project was wrong: it is NULL during File -> Close and points
        // at the wrong project when loading into a non-current project. SProject's
        // destructor deletes its children before tearing down externFileDict_, so
        // the dict is still alive here.
        if( QObject *p = parent() ) {
            static_cast<SProject*>( p )->removeExternObject( fileName_ );
        }
    }
}

SPlainWave::SPlainWave( SProject *project )
    : SExternFile( project ),
      fileName_( "" ),
      inlineRenderer_( NULL ),
      uiOnsets_( std::make_shared<UiOnsetsSlot>() )
{
}

std::shared_ptr<const SPlainWave::UiOnsets> SPlainWave::onsetsForUi() const
{
    if( !uiOnsets_ ) return nullptr;            // pre-ctor defensive
    if( auto cached = std::atomic_load( &uiOnsets_->ptr ) )
        return cached;                          // hit (possibly an empty vector)

    // First call (or post-analysis invalidation): read the "onsets" sidecar
    // ONCE. A miss caches an empty result so paint never re-hits the store.
    // Params-agnostic (loadAny): the import job chose the detector params.
    auto fresh = std::make_shared<UiOnsets>();
    if( cpWave_ ) {
        const twContentHash content = cpWave_->contentHash();
        if( !content.isNull() ) {
            auto reader = twSidecarStore::instance().loadAny(
                content, twAspect::Onsets, twAspect::OnsetsVersion );
            if( reader && reader->info().recordStride == 12
                && reader->info().recordCount > 0 ) {
                const uint64_t n = reader->info().recordCount;
                std::vector<uint8_t> raw( (size_t) n * 12 );
                if( reader->readRecords( raw.data(), 0, n ) ) {
                    fresh->sourceRate = reader->info().sourceRate;
                    fresh->onsets.reserve( (size_t) n );
                    // v3 record: PACKED 12-byte { u64 pos, f32 salience } LE —
                    // the same layout the analysis job writes and the vocoder
                    // parses (twgrainsource.cc).
                    for( uint64_t i = 0; i < n; i++ ) {
                        const uint8_t *r = raw.data() + (size_t) i * 12;
                        uint64_t pos = 0;
                        for( int b = 7; b >= 0; b-- )
                            pos = ( pos << 8 ) | r[b];
                        uint32_t sb = 0;
                        for( int b = 3; b >= 0; b-- )
                            sb = ( sb << 8 ) | r[8 + b];
                        twOnset o;
                        o.pos = pos;
                        memcpy( &o.salience, &sb, 4 );
                        fresh->onsets.push_back( o );
                    }
                }
            }
        }
    }
    std::shared_ptr<const UiOnsets> published = std::move( fresh );
    std::atomic_store( &uiOnsets_->ptr, published );
    return published;
}

SLink *SPlainWave::instantiateFromDomElement( 
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    (void) parent;
    // Ignore other parameters.
    //
    // NB: use the QString directly. Casting QString::data() (QChar*, UTF-16) to
    // const char* truncates the path at the first byte (e.g. "C:/..." -> "C").
    QString fileName = element.attribute( "filename" );
    if( fileName.isEmpty() ) {
        qWarning() << "SPlainWave: missing/empty filename attribute.";
        return NULL;
    }

    // Undo the portable encoding (project-relative / "~" / absolute) against
    // the project file we are loading from.
    SProject &project = projectLoader.getProject();
    QString resolved =
        SFilePathRef::fromStored( fileName, project.projectFilePath() );

    // A relative reference that does NOT resolve next to the project file keeps
    // its raw spelling, so linkToFile() can still apply the older resolution
    // (the .qxa runner's sample base dir, else the working directory). That is
    // what keeps hand-written fixtures — and any project saved before this
    // encoding existed — loading exactly as they did before. An absolute or "~"
    // form has no such second reading and is used as resolved, missing or not,
    // so the failure names the file it actually looked for.
    const bool wasProjectRelative = !fileName.startsWith( QLatin1Char( '~' ) )
                                    && QFileInfo( fileName ).isRelative();
    if( wasProjectRelative && resolved != fileName
        && !QFileInfo::exists( resolved ) ) {
        resolved = fileName;
    }

    // Last-resort recovery: the reference does not resolve, but a same-named
    // file sits next to the project file. Recordings are written INTO the
    // project's own folder, so a project moved/copied as a unit (or opened past
    // a OneDrive "Documents"<->"Dokumente" redirection) keeps its samples beside
    // the .qxp even when the stored absolute/"~" path still names the old
    // location. Adopt that neighbour. Self-healing: the file now resolves next
    // to the project, so the next save re-encodes it project-relative. Skipped
    // when there is no anchor (headless tests without setProjectFilePath), which
    // leaves linkToFile's sample-base-dir resolution untouched.
    if( !QFileInfo::exists( resolved )
        && !project.projectFilePath().isEmpty() ) {
        const QString projectDir =
            QFileInfo( project.projectFilePath() ).absolutePath();
        if( !projectDir.isEmpty() ) {
            const QString candidate = QDir::cleanPath(
                QDir( projectDir ).filePath( QFileInfo( resolved ).fileName() ) );
            if( QFileInfo::exists( candidate ) ) resolved = candidate;
        }
    }
    return project.linkToFile( resolved );
}

// Phase 5e: Page cache implementation

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_splainwave =
    ( SProjectLoader::registerSObjectClass( "SPlainWave",
          SPlainWave::instantiateFromDomElement ), true );

// Extern-file factory for the model (proposal 14, Phase 5): SProject names
// no concrete types; the wave slice provides the WAV-file loader.
static SExternFile *createPlainWaveFile( SProject *project, QString &fileName )
{
    SPlainWave *w = new SPlainWave( project );
    if( w->setWave( fileName ) < 0 ) {
        delete w;
        return nullptr;
    }
    return w;
}
static const bool s_registered_wavefactory =
    ( SProject::registerExternFileFactory( createPlainWaveFile ), true );
