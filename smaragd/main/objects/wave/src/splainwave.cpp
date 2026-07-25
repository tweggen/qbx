
#include <stdio.h>
#include <cstring>

#include <qobject.h>
#include <qwidget.h>
#include <qlabel.h>
#include "qmessagebox.h"

#include "app/model/sappcontext.h"
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
    o << " filename='" << getFileName() << "'";
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
    // Phase 5e: Use page cache for preview (async revalidation model)
    // Try to get cached preview first, fall back to live preview if unavailable

    if (!cpWave_) return -1;

    // Try page cache first (may be stale or invalid, which is acceptable)
    auto page = getCapture(Preview);
    if (page) {
        std::lock_guard<std::mutex> pageLock(page->pageMutex);
        if (page->validAspects & Preview) {
            // Cache has valid preview; copy from page into dest
            const preview_t* pagePreview = reinterpret_cast<const preview_t*>(page->data);
            const offset_t previewCount = CapturePageData::PAGE_SIZE / sizeof(preview_t);

            // Calculate how much preview data to copy into dest
            offset_t copyCount = std::min((offset_t)nProbes, previewCount);
            memcpy(dest, pagePreview, copyCount * sizeof(preview_t));
            return copyCount;
        }
    }

    // Cache not ready; fall back to live preview
    return getStraightPreview(dest, start, length, nProbes);
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
        || qi.sourceFrames != (uint64_t) forLength ) return false;
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
    qi.channels      = 1;   // straight preview folds channel 0 only
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

    std::vector<uint8_t> opBlob, lpBlob;
    op.serialize( opBlob );
    lp.serialize( lpBlob );
    const uint64_t opHash =
        twSidecarStore::hashParams( opBlob.data(), opBlob.size() );
    const uint64_t lpHash =
        twSidecarStore::hashParams( lpBlob.data(), lpBlob.size() );

    // Skip when both aspects already VALIDATE (version-aware — a bare
    // exists() check would keep files with an outdated aspectVersion forever).
    const bool haveOnsets = twSidecarStore::instance().load(
        content, twAspect::Onsets, twAspect::OnsetsVersion, opHash ) != nullptr;
    const bool haveLoudness = twSidecarStore::instance().load(
        content, twAspect::Loudness, twAspect::LoudnessVersion, lpHash ) != nullptr;
    if( haveOnsets && haveLoudness ) return;

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
    reval->scheduleAnalysisJob(
        [wav, flag, project, content, op, lp, opBlob, lpBlob,
         haveOnsets, haveLoudness, rate]() {
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
            }
            flag->store( false, std::memory_order_release );
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
      inlineRenderer_( NULL )
{    
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
    return projectLoader.getProject().linkToFile( fileName );
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
