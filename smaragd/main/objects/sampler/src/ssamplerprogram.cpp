#include "app/objects/sampler/ssamplerprogram.h"

#include "app/model/sappcontext.h"
#include "app/model/sfilepathref.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/persistence/sprojectloader.h"

#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"
#include "tw/plugins/twplugin.h"
#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplerstretch.h"
#include "tw/sampler/twsamplertypes.h"
#include "tw/core/twcontenthash.h"
#include "tw/sidecar/twanalyzers.h"
#include "tw/sidecar/twaspects.h"
#include "tw/sidecar/twqaf.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/sources/twsamplesource.h"

#include <QDebug>
#include <QDir>
#include <QDomElement>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

// --- the generation job (proposal 56 §4.6, Q2) ------------------------------------
//
// One job decodes ONE file into ONE twSamplerSample on its own std::thread.
// Everything the worker touches is in here and is Qt-free (THREADING rule 1:
// no QObject machinery off the main thread; the path is a std::string, the
// decoder twSampleSource is engine code). The worker never calls back: it
// stores the result, sets `done` and notifies `cv`. The main thread finds it
// either by polling `done` (pollJobs_) or by waiting on `cv` (settle()).

struct SSamplerProgram::Gate {
    std::mutex              m;
    std::condition_variable cv;
    bool                    released = false;
    int                     holdMs   = -1;   // < 0: until released
};

// What a linked layer's generation was bounced FROM (Q3): the content
// component, its content epoch when the bounce started, the window and the
// rate. A layer whose asset's key differs from its generation's needs a new
// bounce; one whose key is equal does not, whatever else happened (Q3.2).
struct SSamplerProgram::LinkKey {
    std::shared_ptr<twComponent> comp;      // the content's root component
    std::uint64_t                epoch  = 0;
    offset_t                     base   = 0;   // the window's start, in the content
    length_t                     frames = 0;   // the window's length
    int                          srate  = 0;
    QString                      asset;        // for the log only

    bool sameAs( const LinkKey &o ) const
    {
        return comp == o.comp && epoch == o.epoch && base == o.base && frames == o.frames &&
               srate == o.srate;
    }
};

// A kept, unstretched SOURCE (Q4): what a re-stretch starts from without
// decoding or bouncing again, and what the onset analysis reads. Planar, at
// the source's own rate; `content` keys its onsets (the sidecar store's key).
struct SSamplerProgram::Source {
    int                channels = 0;
    double             rate     = 0.0;
    std::int64_t       frames   = 0;
    std::vector<float> plane[2];
    twContentHash      content;
};

namespace {

// The gate a held job or analysis waits on (set-sampler-render-gate).
void waitOnGate( const std::shared_ptr<SSamplerProgram::Gate> &gate,
                 const std::atomic<bool> &cancelled );

// The onsets the sidecar store holds for `content`, in frames of a source at
// `rate` (params-agnostic, exactly as twGrainSource reads them for its
// vocoder). Null when the store has none; an empty list when it holds an
// analysis that found none.
std::shared_ptr<const std::vector<std::uint64_t>> sidecarOnsets( const twContentHash &content,
                                                                 double rate )
{
    if( content.isNull() || !twSidecarStore::instance().enabled() ) return nullptr;
    auto r = twSidecarStore::instance().loadAny( content, twAspect::Onsets,
                                                 twAspect::OnsetsVersion );
    if( !r || r->info().sourceRate == 0 ) return nullptr;
    auto out = std::make_shared<std::vector<std::uint64_t>>();
    const std::uint64_t n = r->info().recordCount;
    if( n == 0 ) return out;
    if( r->info().recordStride != 12 ) return nullptr;
    std::vector<std::uint8_t> raw( (std::size_t)n * 12 );
    if( !r->readRecords( raw.data(), 0, n ) ) return nullptr;
    const double scale = rate / (double)r->info().sourceRate;
    out->reserve( (std::size_t)n );
    for( std::uint64_t i = 0; i < n; ++i ) {
        std::uint64_t pos = 0;
        for( int b = 7; b >= 0; --b ) pos = ( pos << 8 ) | raw[(std::size_t)i * 12 + (std::size_t)b];
        out->push_back( (std::uint64_t)std::llround( (double)pos * scale ) );
    }
    return out;
}

// A content hash for planes that did not come with one (a bounce): the
// planes' bytes, channel after channel.
twContentHash hashPlanes( const SSamplerProgram::Source &s )
{
    twContentHash h = twHashBuffer( s.plane[0].data(), s.plane[0].size() * sizeof( float ) );
    if( s.channels > 1 )
        h = twHashBuffer( s.plane[1].data(), s.plane[1].size() * sizeof( float ), h.lo ^ h.hi );
    return h;
}

}  // namespace

struct SSamplerProgram::Job {
    std::uint32_t layerId    = 0;
    std::uint64_t generation = 0;
    QString       src;             // main thread only
    std::string   path;            // the resolved file, read by the worker
    tw303aEnvironment *env = nullptr;
    std::shared_ptr<Gate> gate;     // set-sampler-render-gate, or null
    // A BOUNCE (Q3) instead of a decode: the worker freezes key->comp's pages
    // over [key->base, key->base + key->frames). The component is held by the
    // key; the worker touches nothing else. For a RE-STRETCH of a linked layer
    // (sourceIn set) it is only the key the generation is recorded under.
    std::shared_ptr<const LinkKey> key;
    // Q4: what the generation is for (stretched to want.spec, or not), the
    // source to start from instead of decoding or bouncing (a re-stretch),
    // and the onsets to stretch with (null: whatever the sidecar store holds).
    Want                                              want;
    std::shared_ptr<const Source>                     sourceIn;
    std::shared_ptr<const std::vector<std::uint64_t>> onsetsIn;

    std::mutex              m;
    std::condition_variable cv;
    std::atomic<bool>       done{ false };
    std::atomic<bool>       cancelled{ false };
    std::shared_ptr<const audio::twSamplerSample> result;   // under m, once done
    std::string             error;                          // under m, once done
    // Q4 results, under m once done: the source (kept only when want.on),
    // whether the vocoder ran, and the onsets it was given.
    std::shared_ptr<const Source>                     source;
    bool                                              stretched = false;
    std::shared_ptr<const std::vector<std::uint64_t>> onsetsUsed;
    std::chrono::steady_clock::time_point started;
    std::thread             thread;

    void run()
    {
        waitOnGate( gate, cancelled );
        std::shared_ptr<const audio::twSamplerSample> out;
        std::shared_ptr<const Source> srcBuf = sourceIn;
        std::shared_ptr<const std::vector<std::uint64_t>> used;
        bool didStretch = false;
        std::string err;
        if( cancelled.load( std::memory_order_acquire ) ) {
            err = "cancelled";
        } else {
            if( !srcBuf ) srcBuf = key ? bounce( err ) : decode( err );
            if( srcBuf ) out = derive( *srcBuf, used, didStretch, err );
        }
        {
            std::lock_guard<std::mutex> lk( m );
            result     = std::move( out );
            error      = std::move( err );
            source     = want.on ? srcBuf : nullptr;
            stretched  = didStretch;
            onsetsUsed = std::move( used );
            done.store( true, std::memory_order_release );
        }
        cv.notify_all();
    }

    std::shared_ptr<const Source> decode( std::string &err )
    {
        if( !env ) { err = "no audio environment to decode with"; return nullptr; }
        twSampleSource src( *env, QString::fromStdString( path ) );
        const int ch = src.wasLoaded() ? (int)src.channels() : 0;
        if( !src.wasLoaded() ) { err = "cannot decode " + path; return nullptr; }
        if( ch != 1 && ch != 2 ) {
            err = path + " has " + std::to_string( ch ) + " channels; a sample has 1 or 2";
            return nullptr;
        }
        auto s      = std::make_shared<Source>();
        s->channels = ch;
        s->rate     = (double)src.sampleRate();
        s->frames   = (std::int64_t)src.length();
        for( int c = 0; c < ch; ++c ) {
            const float *p = src.channelData( c );
            s->plane[c].assign( p, p + s->frames );
        }
        s->content = src.contentHash();
        if( s->content.isNull() ) s->content = hashPlanes( *s );
        return s;
    }

    // The asset bounce, on this worker (proposal 56 §4.7, B9 as built): the
    // content component's own pages over the window, exactly as SCut::
    // buildCapture_ captures a container on a revalidator worker -- freezePage
    // positions the component itself, under its own cursor mutex, so no seek
    // and no model object is touched here.
    std::shared_ptr<const Source> bounce( std::string &err )
    {
        twComponent &c = *key->comp;
        idx_t ch = c.getOutputChannels();
        if( ch < 1 ) { err = "the asset's component has no output channel"; return nullptr; }
        if( ch > 2 ) ch = 2;   // a sample has 1 or 2 channels; the first two
        const length_t n = key->frames;
        if( n <= 0 ) { err = "the asset's window is empty"; return nullptr; }
        auto s      = std::make_shared<Source>();
        s->channels = (int)ch;
        s->rate     = (double)key->srate;
        s->frames   = (std::int64_t)n;
        for( idx_t k = 0; k < ch; ++k ) s->plane[k].assign( (std::size_t)n, 0.0f );
        length_t done = 0;
        offset_t pos  = (offset_t)key->base;
        while( done < n ) {
            if( cancelled.load( std::memory_order_acquire ) ) { err = "cancelled"; return nullptr; }
            const length_t need = std::min<length_t>( n - done,
                                                      (length_t)twOutputPage::FRAME_CAPACITY );
            std::shared_ptr<twOutputPage> page =
                c.freezePage( pos, nullptr, 0, need, key->srate, nullptr );
            if( !page || page->validFrames == 0 ) break;
            const length_t got = std::min<length_t>( std::min<length_t>( (length_t)page->validFrames,
                                                                         need ),
                                                     (length_t)page->channelFrames() );
            for( idx_t k = 0; k < ch; ++k ) {
                const float *src = page->channelPtr( twPageClampChannel( *page, k ) );
                std::copy( src, src + got, s->plane[k].data() + done );
            }
            done += got;
            pos  += got;
            if( got < need ) break;
        }
        s->content = hashPlanes( *s );
        return s;
    }

    // The generation's buffer from a source: the source itself, or (Q4) the
    // source stretched to the tempo by the vocoder, with the factor as its
    // time scale. At a factor of exactly 1 (the layer's own tempo) the source
    // plays as it is; beats = 0 cannot be stretched and plays as it is too.
    std::shared_ptr<const audio::twSamplerSample> derive(
        const Source &s, std::shared_ptr<const std::vector<std::uint64_t>> &used, bool &didStretch,
        std::string &err )
    {
        const float *planar[2] = { s.plane[0].data(), s.channels > 1 ? s.plane[1].data() : nullptr };
        if( want.on ) {
            const double f = audio::twSamplerStretchFactor( want.spec, s.frames, s.rate );
            if( f > 0.0 && f != 1.0 ) {
                if( cancelled.load( std::memory_order_acquire ) ) { err = "cancelled"; return nullptr; }
                used = onsetsIn ? onsetsIn : sidecarOnsets( s.content, s.rate );
                didStretch = true;
                return audio::twSamplerStretch( s.channels, s.rate, planar, s.frames, f,
                                                used && !used->empty() ? used->data() : nullptr,
                                                used ? used->size() : 0, generation, &err );
            }
        }
        return audio::twSamplerSample::fromPlanar( s.channels, s.rate, planar, s.frames, generation,
                                                   &err );
    }

    void cancel()
    {
        cancelled.store( true, std::memory_order_release );
        if( gate ) {
            { std::lock_guard<std::mutex> lk( gate->m ); }
            gate->cv.notify_all();
        }
    }

    void join()
    {
        if( thread.joinable() ) thread.join();
    }
};

// ONE onset analysis of a kept source (Q4; DESIGN §5: the vocoder places its
// transient keyframes from the source's onsets). Runs twDetectOnsets with the
// parameters SPlainWave's import analysis uses, on its own std::thread, and
// stores the result in the sidecar store under the source's content hash, so
// a later decode (in this process or the next) finds it. Qt-free; it never
// calls back (the main thread polls `done`, or waits on `cv`).
struct SSamplerProgram::AnalysisJob {
    std::uint32_t                 layerId = 0;
    std::shared_ptr<const Source> source;
    std::shared_ptr<Gate>         gate;

    std::mutex              m;
    std::condition_variable cv;
    std::atomic<bool>       done{ false };
    std::atomic<bool>       cancelled{ false };
    std::shared_ptr<const std::vector<std::uint64_t>> result;   // under m, once done
    std::thread             thread;

    void run()
    {
        waitOnGate( gate, cancelled );
        std::shared_ptr<std::vector<std::uint64_t>> out;
        if( !cancelled.load( std::memory_order_acquire ) ) {
            const Source &s     = *source;
            const uint32_t rate = (uint32_t)std::lround( s.rate );
            twOnsetParams op;
            op.minSeparationFrames = rate * 3 / 100;   // ~30 ms, as SPlainWave
            const float *chans[2] = { s.plane[0].data(),
                                      s.channels > 1 ? s.plane[1].data() : nullptr };
            std::vector<twOnset> found =
                twDetectOnsets( chans, (uint32_t)s.channels, (uint64_t)s.frames, op );
            out = std::make_shared<std::vector<std::uint64_t>>();
            out->reserve( found.size() );
            std::vector<std::uint8_t> payload;
            payload.reserve( found.size() * 12 );
            for( const twOnset &o : found ) {
                out->push_back( o.pos );
                // v3 records: PACKED 12-byte {u64 pos, f32 salience} LE.
                for( int b = 0; b < 8; ++b ) payload.push_back( (std::uint8_t)( o.pos >> ( 8 * b ) ) );
                std::uint32_t sb;
                std::memcpy( &sb, &o.salience, 4 );
                for( int b = 0; b < 4; ++b ) payload.push_back( (std::uint8_t)( sb >> ( 8 * b ) ) );
            }
            if( twSidecarStore::instance().enabled() && !s.content.isNull() ) {
                std::vector<std::uint8_t> blob;
                op.serialize( blob );
                twQafInfo qi;
                qi.contentHash   = s.content;
                qi.sourceRate    = rate;
                qi.channels      = (uint32_t)s.channels;
                qi.sourceFrames  = (uint64_t)s.frames;
                qi.aspectId      = twAspect::Onsets;
                qi.aspectVersion = twAspect::OnsetsVersion;
                qi.recordStride  = 12;
                qi.recordCount   = (uint64_t)found.size();
                qi.hopFrames     = op.hop;
                qi.params        = blob;
                twSidecarStore::instance().store( qi, payload.data(), (uint64_t)payload.size() );
            }
        }
        {
            std::lock_guard<std::mutex> lk( m );
            result = std::move( out );
            done.store( true, std::memory_order_release );
        }
        cv.notify_all();
    }

    void cancel()
    {
        cancelled.store( true, std::memory_order_release );
        if( gate ) {
            { std::lock_guard<std::mutex> lk( gate->m ); }
            gate->cv.notify_all();
        }
    }

    void join()
    {
        if( thread.joinable() ) thread.join();
    }
};

namespace {

void waitOnGate( const std::shared_ptr<SSamplerProgram::Gate> &gate,
                 const std::atomic<bool> &cancelled )
{
    if( !gate ) return;
    std::unique_lock<std::mutex> lk( gate->m );
    auto released = [&] {
        return gate->released || cancelled.load( std::memory_order_acquire );
    };
    if( gate->holdMs >= 0 )
        gate->cv.wait_for( lk, std::chrono::milliseconds( gate->holdMs ), released );
    else
        gate->cv.wait( lk, released );
}

}  // namespace

namespace {

// `file:<path>` -> an absolute path, the way linkToFile / the MIDI file verbs
// resolve an input path: absolute as given; next to the action script (the
// project's sample base dir) when it exists there; next to the project file
// (SFilePathRef's stored forms: relative, ~/, absolute); else the cwd.
QString resolveFileSrc( SProject *project, const QString &src )
{
    const QString given = src.mid( 5 );   // after "file:"
    if( given.isEmpty() ) return QString();
    if( QFileInfo( given ).isAbsolute() ) return QDir::cleanPath( given );
    if( project ) {
        const QString base = project->sampleBaseDir();
        if( !base.isEmpty() ) {
            const QString c = QDir::cleanPath( QDir( base ).filePath( given ) );
            if( QFileInfo::exists( c ) ) return c;
        }
        if( !project->projectFilePath().isEmpty() ) {
            const QString c = SFilePathRef::fromStored( given, project->projectFilePath() );
            if( QFileInfo::exists( c ) ) return c;
        }
    }
    return QDir::cleanPath( QDir::current().filePath( given ) );
}

bool isFileSrc( const std::string &src )
{
    return src.size() > 5 && src.compare( 0, 5, "file:" ) == 0;
}

// `link:<n>`, n a positive integer (Q3).
bool parseLinkSrc( const std::string &src, std::uint32_t &n )
{
    if( src.size() <= 5 || src.compare( 0, 5, "link:" ) != 0 ) return false;
    bool ok = false;
    const qulonglong v = QString::fromStdString( src.substr( 5 ) ).toULongLong( &ok );
    if( !ok || v == 0 || v > 0xFFFFFFFFull ) return false;
    n = (std::uint32_t)v;
    return true;
}

bool isLinkSrc( const std::string &src )
{
    std::uint32_t n = 0;
    return parseLinkSrc( src, n );
}

// A finished job is installed only when no offline render is running (DESIGN
// §6.1 rule 3, proposal 56 invariant 6). Without an application (a unit test)
// nothing renders.
bool renderRunning()
{
    return SAppContext::hasInstance() && SAppContext::get().isRenderingActive();
}

}  // namespace

SSamplerProgram::SSamplerProgram( SProject *project ) : SObject( project )
{
    setSName( QStringLiteral( "NassauDigital program" ) );
    // THE TRIGGER of a linked layer's bounce (DESIGN §6.2): qbx signals after
    // EVERY action; checkLinks_ costs one compare per linked layer and acts
    // only on an asset whose own content epoch moved.
    if( project ) {
        connect( project, &SProject::arrangementChanged, this,
                 &SSamplerProgram::onArrangementChanged_ );
        // THE TRIGGER of a stretch layer's re-stretch (DESIGN §5, Q4): the
        // tempo map's one writer (set-tempo, and the loader) emits it.
        connect( project, &SProject::bpmTempoChanged, this,
                 [this]( double ) { onTempoChanged_(); } );
    }
}

SSamplerProgram::~SSamplerProgram()
{
    // Every worker is joined here, on the main thread: a held one is released
    // by cancel(), a decoding one finishes its file. No worker outlives the
    // object it reports to (it reports to nothing, but its Job is shared).
    for( auto &kv : jobs_ ) kv.second->cancel();
    for( auto &j : retiredJobs_ ) j->cancel();
    for( auto &kv : analysisJobs_ ) kv.second->cancel();
    for( auto &j : retiredAnalyses_ ) j->cancel();
    for( auto &kv : jobs_ ) kv.second->join();
    for( auto &j : retiredJobs_ ) j->join();
    for( auto &kv : analysisJobs_ ) kv.second->join();
    for( auto &j : retiredAnalyses_ ) j->join();
}

// --- the program ---------------------------------------------------------------

bool SSamplerProgram::edit( const std::function<bool( audio::twSamplerProgramModel & )> &fn,
                            QString *why )
{
    if( !isReadable() ) {
        if( why )
            *why = QStringLiteral( "the stored program could not be read (%1); it is kept "
                                   "verbatim and is not editable" ).arg( loadError_ );
        return false;
    }
    // A COPY, so a refusal anywhere inside `fn` leaves the program exactly as
    // it was even if `fn` made more than one core call before refusing.
    audio::twSamplerProgramModel work = model_;
    if( !fn( work ) ) return false;
    model_ = std::move( work );
    publish_();
    reconcileSamples_();
    emit programChanged();
    return true;
}

// --- the selection ---------------------------------------------------------------

QVector<std::uint32_t> SSamplerProgram::selectedZones() const
{
    QVector<std::uint32_t> out;
    for( std::uint32_t id : selection_ )
        if( model_.hasZone( id ) ) out.append( id );
    return out;
}

void SSamplerProgram::setSelection( const QVector<std::uint32_t> &ids )
{
    if( ids == selection_ ) return;
    selection_ = ids;
    emit selectionChanged();
}

// --- the view, the pad, the record input, the last key (Q6) ----------------------

QString SSamplerProgram::viewText( View v )
{
    switch( v ) {
    case View::Key:  return QStringLiteral( "key" );
    case View::Pad:  return QStringLiteral( "pad" );
    case View::Map:  return QStringLiteral( "map" );
    case View::Zone: return QStringLiteral( "zone" );
    }
    return QStringLiteral( "key" );
}

bool SSamplerProgram::viewFromText( const QString &text, View &out )
{
    if( text == QLatin1String( "key" ) )  { out = View::Key;  return true; }
    if( text == QLatin1String( "pad" ) )  { out = View::Pad;  return true; }
    if( text == QLatin1String( "map" ) )  { out = View::Map;  return true; }
    if( text == QLatin1String( "zone" ) ) { out = View::Zone; return true; }
    return false;
}

void SSamplerProgram::setViewState( View view, int padBank, int pad )
{
    if( !padInRange( padBank, pad ) ) return;
    if( view == view_ && padBank == padBank_ && pad == pad_ ) return;
    view_    = view;
    padBank_ = padBank;
    pad_     = pad;
    emit viewChanged();
}

namespace {
// `audio:<device>:<mask>`: the LAST colon separates the mask, as STrack reads a
// track input, so a device name may itself hold colons.
bool splitRecordInput( const QString &spec, QString *device, unsigned *mask )
{
    if( !spec.startsWith( QLatin1String( "audio:" ) ) ) return false;
    const QString rest  = spec.mid( 6 );
    const int     colon = rest.lastIndexOf( QLatin1Char( ':' ) );
    if( colon < 0 ) return false;
    bool ok = false;
    const unsigned m = rest.mid( colon + 1 ).toUInt( &ok, 16 );
    if( !ok || m == 0 ) return false;
    if( device ) *device = rest.left( colon );
    if( mask ) *mask = m;
    return true;
}
}  // namespace

bool SSamplerProgram::isValidRecordInput( const QString &spec )
{
    return spec.isEmpty() || splitRecordInput( spec, nullptr, nullptr );
}

void SSamplerProgram::setRecordInput( const QString &spec )
{
    if( spec == recordInput_ || !isValidRecordInput( spec ) ) return;
    recordInput_ = spec;
    emit recordInputChanged();
}

QString SSamplerProgram::recordInputDevice() const
{
    QString d;
    return splitRecordInput( recordInput_, &d, nullptr ) ? d : QString();
}

unsigned SSamplerProgram::recordInputMask() const
{
    unsigned m = 0;
    return splitRecordInput( recordInput_, nullptr, &m ) ? m : 0u;
}

bool SSamplerProgram::selectPadByKey( int key )
{
    if( key < 0 || key > 127 || !isReadable() ) return false;
    const int n = model_.padSelectedByKey( key, padNumber() );
    // -1: no pad plays the key. A pad past the last bank cannot be the
    // lowest one playing a key unless every pad playing it is, so nothing
    // selectable plays the key then either.
    if( n < 0 || n >= kPadBanks * kPadsPerBank ) return false;
    const int bank = n / kPadsPerBank;
    const int pad  = n % kPadsPerBank;
    if( bank == padBank_ && pad == pad_ ) return false;
    setViewState( view_, bank, pad );
    return true;
}

void SSamplerProgram::setLastKey( int key )
{
    if( key < 0 || key > 127 ) return;
    lastKey_ = key;
}

// --- the hand-over ---------------------------------------------------------------

SSamplerProgram::InstanceInitializer SSamplerProgram::futureInitializer_() const
{
    // The program, then EVERY current generation: what a fresh instance needs
    // to be born current (invariant 4). Captured by value, like the program.
    std::shared_ptr<const audio::twSamplerProgram> snap = snapshot_;
    std::vector<std::pair<std::uint32_t, std::shared_ptr<const audio::twSamplerSample>>> samples;
    for( const auto &kv : installed_ ) samples.emplace_back( kv.first, kv.second.sample );
    return [snap, samples]( audio::twPlugin &p ) {
        if( audio::twSamplerHost *h = dynamic_cast<audio::twSamplerHost *>( &p ) ) {
            h->setProgram( snap );
            for( const auto &s : samples ) h->setSample( s.first, s.second );
        }
    };
}

SSamplerProgram::InstanceInitializer SSamplerProgram::instanceInitializer() const
{
    // The snapshot is captured BY VALUE: the initializer the slot keeps for its
    // next rebuild installs exactly the program that was current when it was
    // handed over, and a later edit hands over a new one.
    std::shared_ptr<const audio::twSamplerProgram> snap = snapshot_;
    return [snap]( audio::twPlugin &p ) {
        if( audio::twSamplerHost *h = dynamic_cast<audio::twSamplerHost *>( &p ) )
            h->setProgram( snap );
    };
}

void SSamplerProgram::setInstaller( Installer installer )
{
    installer_ = std::move( installer );
    if( installer_ ) {
        publish_();
        // A program that arrives from a file (or is re-bound) loads its file
        // layers now: binding is where an instance first needs them.
        reconcileSamples_( true );
    }
}

void SSamplerProgram::publish_()
{
    // An unreadable program plays the EMPTY program (the default model), never
    // a stale one.
    snapshot_        = model_.snapshot();
    publishedSerial_ = snapshot_ ? snapshot_->serial() : 0;
    // `now` installs the program only (Q1's behaviour): the buffers an
    // instance holds are keyed by layer id and stay where they are.
    if( installer_ ) installer_( futureInitializer_(), instanceInitializer() );
}

// --- generations ---------------------------------------------------------------

SSamplerProgram::LayerState SSamplerProgram::layerState( std::uint32_t layerId ) const
{
    LayerState st;
    auto in = installed_.find( layerId );
    if( in != installed_.end() ) {
        st.installedGeneration = in->second.generation;
        st.installedSrc        = in->second.src;
        st.timeScale           = in->second.sample ? in->second.sample->timeScale() : 1.0;
        st.stretched           = in->second.stretched;
        st.onsets = in->second.stretched && in->second.onsets ? (int)in->second.onsets->size() : -1;
    }
    st.analyzing = analysisJobs_.count( layerId ) != 0;
    auto an = analyses_.find( layerId );
    if( an != analyses_.end() ) st.analyses = an->second;
    auto jb = jobs_.find( layerId );
    if( jb != jobs_.end() ) {
        st.pendingGeneration = jb->second->generation;
        st.pendingSrc        = jb->second->src;
    }
    auto n = installs_.find( layerId );
    if( n != installs_.end() ) st.installs = n->second;
    auto r = renders_.find( layerId );
    if( r != renders_.end() ) st.renders = r->second;
    st.bounceDue = due_.count( layerId ) != 0;
    return st;
}

int SSamplerProgram::outstandingGenerations() const
{
    return (int)jobs_.size();
}

void SSamplerProgram::reconcileSamples_( bool atBind )
{
    // Pin exactly the links a layer names (LinkRec), bound or not.
    syncLinkPins_();
    // Nothing to feed while unbound: bind (setInstaller) reconciles.
    if( !installer_ ) return;
    bool bindDue = false;

    std::set<std::uint32_t> layers;
    std::vector<std::uint32_t> removed;
    for( std::uint32_t z : model_.zoneIds() ) {
        for( std::uint32_t id : model_.layerIds( z ) ) {
            layers.insert( id );
            std::string src;
            model_.value( audio::twSamplerScope::Layer, id, "src", &src );
            const bool link = isLinkSrc( src );
            const QString want = ( isFileSrc( src ) || link ) ? QString::fromStdString( src )
                                                               : QString();

            auto jb = jobs_.find( id );
            if( jb != jobs_.end() ) {
                if( jb->second->src == want ) continue;   // already on its way
                cancelJob_( id );                         // superseded
            }
            if( !link ) due_.erase( id );
            auto in = installed_.find( id );
            const QString have = in != installed_.end() ? in->second.src : QString();
            // Same text: a file is done; a link may still have MOVED, which
            // is checkLinks_'s question (an epoch, not a text).
            if( want == have ) continue;
            if( want.isEmpty() ) {
                installed_.erase( id );
                removed.push_back( id );
            } else if( link && atBind ) {
                // BOUND (a program read from a file, or re-bound): DUE, not
                // started. A load goes on invalidating after it binds, so a
                // bounce started here would be superseded by the next
                // arrangementChanged (measured: two bounces per load). The
                // debounce -- or a settle -- starts it once the load is done.
                due_.insert( id );
                bindDue = true;
            } else if( link ) {
                // A NEW link (or a link put back by an undo): bounced at once,
                // no debounce -- the debounce is for an asset edited under a
                // standing link.
                due_.erase( id );
                LinkKey key;
                QString why;
                if( linkKeyOf_( want, key, &why ) ) {
                    startBounce_( id, want, key );
                } else {
                    qWarning().noquote()
                        << QStringLiteral( "NassauDigital: layer %1: %2 cannot be bounced (%3); "
                                           "the layer plays nothing" )
                               .arg( id ).arg( want, why );
                    if( in != installed_.end() ) {
                        installed_.erase( in );
                        removed.push_back( id );
                    }
                }
            } else {
                startJob_( id, want );
            }
        }
    }
    // A layer that is gone takes its buffer and its job with it.
    for( auto it = installed_.begin(); it != installed_.end(); ) {
        if( !layers.count( it->first ) ) {
            removed.push_back( it->first );
            it = installed_.erase( it );
        } else {
            ++it;
        }
    }
    std::vector<std::uint32_t> gone;
    for( const auto &kv : jobs_ )
        if( !layers.count( kv.first ) ) gone.push_back( kv.first );
    for( std::uint32_t id : gone ) cancelJob_( id );
    for( auto it = due_.begin(); it != due_.end(); )
        it = layers.count( *it ) ? std::next( it ) : due_.erase( it );

    if( !removed.empty() ) installChanged_( removed );
    if( bindDue ) checkLinks_( false );   // arms the debounce
    // An edit of a zone's mode or method, or of a layer's beats or positions,
    // changes what a stretch layer's generation should be without changing
    // its src (Q4): compared here, after every committed edit.
    checkStretch_();
}

// --- asset links (Q3) ---------------------------------------------------------------

int SSamplerProgram::bounceDebounceMs()
{
    // MEASURED CHOICE (objects/sampler/CONTRACT.md inv. 18): long enough that
    // the burst of actions one gesture submits (a drag commits at UI rate,
    // 16-60 ms apart) costs ONE bounce, short enough that a release-and-play
    // hears the edit. An offline render never waits for it: the settle starts
    // a due bounce at once.
    int ms = 150;
    bool ok = false;
    const int v = qEnvironmentVariableIntValue( "SMARAGD_SAMPLER_BOUNCE_DEBOUNCE_MS", &ok );
    if( ok && v >= 0 ) ms = v;
    return ms;
}

std::uint32_t SSamplerProgram::linkAsset( SObject *body, std::uint32_t reuse )
{
    if( !body ) return 0;
    std::uint32_t id = 0;
    if( reuse != 0 ) {
        auto it = links_.find( reuse );
        if( it != links_.end() && it->second.body == body ) id = reuse;
    }
    if( id == 0 ) {
        id = nextLinkId_++;
        while( links_.count( id ) ) id = nextLinkId_++;   // cannot happen; never reuse
        links_[id].body = body;
    }
    // An SLink CHILD (invariant 9): it holds the body by object -- a rename
    // changes nothing it holds -- and pins it, and the project file resolves
    // it by object id like any other link. Pinned NOW, before the edit that
    // names it, so the body cannot go while the edit runs.
    LinkRec &r = links_[id];
    if( !r.pin ) r.pin = new SLink( *body, this );
    return id;
}

void SSamplerProgram::forgetLink( std::uint32_t linkId )
{
    auto it = links_.find( linkId );
    if( it == links_.end() || namedLinks_().count( linkId ) ) return;
    delete it->second.pin;
    links_.erase( it );
}

std::set<std::uint32_t> SSamplerProgram::namedLinks_() const
{
    std::set<std::uint32_t> named;
    for( std::uint32_t z : model_.zoneIds() )
        for( std::uint32_t id : model_.layerIds( z ) ) {
            std::string src;
            model_.value( audio::twSamplerScope::Layer, id, "src", &src );
            std::uint32_t n = 0;
            if( parseLinkSrc( src, n ) ) named.insert( n );
        }
    return named;
}

void SSamplerProgram::syncLinkPins_()
{
    const std::set<std::uint32_t> named = namedLinks_();
    for( auto &kv : links_ ) {
        LinkRec &r = kv.second;
        const bool want = named.count( kv.first ) && r.body;
        if( want && !r.pin ) {
            r.pin = new SLink( *r.body, this );
        } else if( !want && r.pin ) {
            delete r.pin;
            r.pin = nullptr;
        }
    }
}

SObject *SSamplerProgram::linkTarget( std::uint32_t linkId ) const
{
    auto it = links_.find( linkId );
    return it != links_.end() ? it->second.body.data() : nullptr;
}

QList<SObject *> SSamplerProgram::liveLinkTargets() const
{
    QList<SObject *> out;
    for( std::uint32_t z : model_.zoneIds() ) {
        for( std::uint32_t id : model_.layerIds( z ) ) {
            std::string src;
            model_.value( audio::twSamplerScope::Layer, id, "src", &src );
            std::uint32_t n = 0;
            if( !parseLinkSrc( src, n ) ) continue;
            SObject *b = linkTarget( n );
            if( b && !out.contains( b ) ) out.append( b );
        }
    }
    return out;
}

bool SSamplerProgram::linkKeyOf_( const QString &src, LinkKey &out, QString *why ) const
{
    std::uint32_t n = 0;
    if( !parseLinkSrc( src.toStdString(), n ) ) {
        if( why ) *why = QStringLiteral( "not a link" );
        return false;
    }
    SObject *body = linkTarget( n );
    if( !body ) {
        if( why ) *why = QStringLiteral( "this program has no link %1" ).arg( n );
        return false;
    }
    // The body is a WINDOW over its content (an asset is an SCut over a
    // container); the bounce renders the CONTENT over the window, because the
    // body's own reader plays a capture that is rebuilt asynchronously and may
    // be stale (SCut::buildCapture_). windowStep is the model's generic way
    // to ask a window where it starts, without naming objects/cut.
    SObject::SWindowStep step;
    SObject *content = body;
    offset_t base    = 0;
    if( body->windowStep( 0, step ) && step.content ) {
        content = step.content;
        base    = step.pos;
    }
    const length_t frames = body->getDurationBlocking();
    std::shared_ptr<twComponent> comp = content->resolveClip( 0 ).component;
    if( !comp ) {
        if( why ) *why = QStringLiteral( "its content has no component" );
        return false;
    }
    if( frames <= 0 ) {
        if( why ) *why = QStringLiteral( "its window is empty" );
        return false;
    }
    SProject *proj = getProjectSafe();
    out.comp   = comp;
    out.epoch  = comp->contentEpochNow();
    out.base   = base;
    out.frames = frames;
    out.srate  = SAppContext::hasInstance() && SAppContext::get().get303aEnvironment()
                     ? (int)SAppContext::get().get303aEnvironment()->getSRate() : 48000;
    out.asset  = proj ? proj->assetNameOf( body ) : QString();
    return true;
}

void SSamplerProgram::startBounce_( std::uint32_t layerId, const QString &src, const LinkKey &key )
{
    cancelJob_( layerId );
    due_.erase( layerId );
    auto job        = std::make_shared<Job>();
    job->layerId    = layerId;
    job->generation = ++lastGeneration_[layerId];
    job->src        = src;
    job->key        = std::make_shared<const LinkKey>( key );
    job->want       = wantFor_( layerId );
    auto g = armedGates_.find( layerId );
    if( g != armedGates_.end() ) {
        job->gate = g->second;
        liveGates_[layerId] = g->second;
        armedGates_.erase( g );
    }
    job->started = std::chrono::steady_clock::now();
    ++renders_[layerId];
    Job *raw = job.get();
    job->thread = std::thread( [raw] { raw->run(); } );
    jobs_[layerId] = job;
    qInfo().noquote() << QStringLiteral( "NassauDigital: layer %1: generation %2 rendering: "
                                         "bounce %3 of asset \"%4\" (content epoch %5, "
                                         "%6 frames from %7)%8" )
                             .arg( layerId ).arg( (qulonglong)job->generation )
                             .arg( renders_[layerId] ).arg( key.asset )
                             .arg( (qulonglong)key.epoch ).arg( (qlonglong)key.frames )
                             .arg( (qlonglong)key.base )
                             .arg( job->gate ? QStringLiteral( " (HELD by the render gate)" )
                                             : QString() );
    ensurePolling_();
}

void SSamplerProgram::checkLinks_( bool start )
{
    if( !installer_ ) return;
    for( std::uint32_t z : model_.zoneIds() ) {
        for( std::uint32_t id : model_.layerIds( z ) ) {
            std::string s;
            model_.value( audio::twSamplerScope::Layer, id, "src", &s );
            if( !isLinkSrc( s ) ) continue;
            const QString src = QString::fromStdString( s );
            // What the layer's newest generation was (or is being) bounced from.
            std::shared_ptr<const LinkKey> have;
            auto jb = jobs_.find( id );
            if( jb != jobs_.end() && jb->second->src == src ) {
                have = jb->second->key;
            } else {
                auto in = installed_.find( id );
                if( in != installed_.end() && in->second.src == src ) have = in->second.key;
            }
            LinkKey now;
            if( !linkKeyOf_( src, now, nullptr ) ) continue;   // keeps what it has
            // ONE COMPARE per linked layer: an action that did not touch the
            // asset leaves its epoch, and so its key, where it was (Q3.2).
            if( have && have->sameAs( now ) ) {
                due_.erase( id );
                continue;
            }
            if( start ) startBounce_( id, src, now );
            else        due_.insert( id );
        }
    }
    if( start || due_.empty() ) {
        if( debounce_ ) debounce_->stop();
        return;
    }
    if( !debounce_ ) {
        debounce_ = new QTimer( this );
        debounce_->setSingleShot( true );
        connect( debounce_, &QTimer::timeout, this, [this] {
            // Never start a bounce under a running offline render: its pages
            // would be frozen beside the render's. Try again after it.
            if( renderRunning() ) { debounce_->start( bounceDebounceMs() ); return; }
            checkLinks_( true );
        } );
    }
    // TRAILING: every further move restarts it, so a burst costs one bounce.
    debounce_->start( bounceDebounceMs() );
}

void SSamplerProgram::flushDueBounces()
{
    checkLinks_( true );
}

void SSamplerProgram::onArrangementChanged_()
{
    checkLinks_( false );
}

void SSamplerProgram::startJob_( std::uint32_t layerId, const QString &src,
                                 std::shared_ptr<const Source> from )
{
    cancelJob_( layerId );   // a job for the same layer is superseded
    auto job        = std::make_shared<Job>();
    job->layerId    = layerId;
    job->generation = ++lastGeneration_[layerId];
    job->src        = src;
    job->want       = wantFor_( layerId );
    if( from ) {
        // A RE-STRETCH (Q4): from the kept source, with the onsets known for
        // it (null: the worker asks the sidecar store). A linked layer keeps
        // the key its source was bounced from, so checkLinks_ still compares.
        job->sourceIn = from;
        job->onsetsIn = knownOnsets_( *from );
        auto in = installed_.find( layerId );
        if( in != installed_.end() ) job->key = in->second.key;
    } else {
        job->path = resolveFileSrc( getProjectSafe(), src ).toStdString();
    }
    job->env        = SAppContext::hasInstance() ? SAppContext::get().get303aEnvironment()
                                                 : nullptr;
    auto g = armedGates_.find( layerId );
    if( g != armedGates_.end() ) {
        job->gate = g->second;
        liveGates_[layerId] = g->second;
        armedGates_.erase( g );
    }
    job->started = std::chrono::steady_clock::now();
    Job *raw = job.get();
    job->thread = std::thread( [raw] { raw->run(); } );
    jobs_[layerId] = job;
    const QString what =
        from ? QStringLiteral( "a re-stretch of its kept source (%1 BPM, %2 beats)" )
                   .arg( job->want.spec.bpm ).arg( job->want.spec.beats )
             : QString::fromStdString( job->path );
    qInfo().noquote() << QStringLiteral( "NassauDigital: layer %1: generation %2 rendering from "
                                         "%3%4" )
                             .arg( layerId ).arg( (qulonglong)job->generation )
                             .arg( what )
                             .arg( job->gate ? QStringLiteral( " (HELD by the render gate)" )
                                             : QString() );
    ensurePolling_();
}

void SSamplerProgram::cancelJob_( std::uint32_t layerId )
{
    auto jb = jobs_.find( layerId );
    if( jb == jobs_.end() ) return;
    jb->second->cancel();
    retiredJobs_.push_back( jb->second );
    jobs_.erase( jb );
    liveGates_.erase( layerId );
    ensurePolling_();
}

int SSamplerProgram::installFinished_( bool force )
{
    if( !force && renderRunning() ) return 0;   // pinned: the render keeps its generations

    std::vector<std::uint32_t> changed;
    for( auto it = jobs_.begin(); it != jobs_.end(); ) {
        Job &j = *it->second;
        if( !j.done.load( std::memory_order_acquire ) ) { ++it; continue; }
        j.join();
        std::shared_ptr<const audio::twSamplerSample> sample;
        std::string err;
        std::shared_ptr<const Source> source;
        std::shared_ptr<const std::vector<std::uint64_t>> used;
        bool stretched = false;
        {
            std::lock_guard<std::mutex> lk( j.m );
            sample    = j.result;
            err       = j.error;
            source    = j.source;
            used      = j.onsetsUsed;
            stretched = j.stretched;
        }
        if( sample ) {
            Installed rec{ j.generation, j.src, sample, j.key };
            rec.want      = j.want;
            rec.source    = source;
            rec.stretched = stretched;
            rec.onsets    = used;
            installed_[j.layerId] = std::move( rec );
            // Onsets the worker found in the sidecar store are known now.
            if( used && source && !source->content.isNull() )
                onsets_[{ source->content.hi, source->content.lo }] = used;
            ++installs_[j.layerId];
            changed.push_back( j.layerId );
            if( stretched )
                qInfo().noquote()
                    << QStringLiteral( "NassauDigital: layer %1: generation %2 is stretched x%3 "
                                       "(%4 BPM) with %5" )
                           .arg( j.layerId ).arg( (qulonglong)j.generation )
                           .arg( sample->timeScale(), 0, 'g', 12 ).arg( j.want.spec.bpm )
                           .arg( used ? QStringLiteral( "%1 onset keyframe(s)" ).arg( used->size() )
                                      : QStringLiteral( "the vocoder's fixed keyframe grid "
                                                        "(no onsets known yet)" ) );
        } else {
            // The layer keeps what it had: a failed decode is not a new
            // generation, and silence pretending to be one would be worse.
            qWarning().noquote()
                << QStringLiteral( "NassauDigital: layer %1: generation %2 FAILED (%3); the "
                                   "layer keeps generation %4" )
                       .arg( j.layerId ).arg( (qulonglong)j.generation )
                       .arg( QString::fromStdString( err ) )
                       .arg( (qulonglong)( installed_.count( j.layerId )
                                               ? installed_[j.layerId].generation : 0 ) );
        }
        liveGates_.erase( j.layerId );
        it = jobs_.erase( it );
    }
    if( !changed.empty() ) installChanged_( changed );
    for( std::uint32_t id : changed ) maybeAnalyze_( id );
    return (int)changed.size();
}

void SSamplerProgram::installChanged_( const std::vector<std::uint32_t> &layers )
{
    // ONE install for every layer that changed: one param-epoch bump, one
    // stale of the path above the slot (DESIGN §6.1 rule 2).
    std::vector<std::pair<std::uint32_t, std::shared_ptr<const audio::twSamplerSample>>> now;
    for( std::uint32_t id : layers ) {
        auto in = installed_.find( id );
        now.emplace_back( id, in != installed_.end() ? in->second.sample : nullptr );
    }
    if( installer_ ) {
        installer_( futureInitializer_(), [now]( audio::twPlugin &p ) {
            if( audio::twSamplerHost *h = dynamic_cast<audio::twSamplerHost *>( &p ) ) {
                for( const auto &s : now ) h->setSample( s.first, s.second );
                // Free what earlier installs retired and no voice reads any
                // more: here, under the processor's mutex, on the main thread
                // -- never process() (invariant 5). A buffer replaced just now
                // is still parked; the next install frees it.
                h->collectRetired();
            }
        } );
    }
    for( const auto &s : now ) {
        const std::uint64_t g = s.second ? s.second->generation() : 0;
        qInfo().noquote() << QStringLiteral( "NassauDigital: layer %1: generation %2 installed" )
                                 .arg( s.first ).arg( (qulonglong)g );
        emit generationInstalled( s.first, g );
    }
}

void SSamplerProgram::ensurePolling_()
{
    if( jobs_.empty() && retiredJobs_.empty() && analysisJobs_.empty() &&
        retiredAnalyses_.empty() ) {
        if( poll_ ) poll_->stop();
        return;
    }
    if( !poll_ ) {
        poll_ = new QTimer( this );
        poll_->setInterval( 10 );
        connect( poll_, &QTimer::timeout, this, &SSamplerProgram::pollJobs_ );
    }
    if( !poll_->isActive() ) poll_->start();
}

void SSamplerProgram::pollJobs_()
{
    for( auto it = retiredJobs_.begin(); it != retiredJobs_.end(); ) {
        if( ( *it )->done.load( std::memory_order_acquire ) ) {
            ( *it )->join();
            it = retiredJobs_.erase( it );
        } else {
            ++it;
        }
    }
    for( auto it = retiredAnalyses_.begin(); it != retiredAnalyses_.end(); ) {
        if( ( *it )->done.load( std::memory_order_acquire ) ) {
            ( *it )->join();
            it = retiredAnalyses_.erase( it );
        } else {
            ++it;
        }
    }
    installFinished_( false );
    processAnalyses_();
    ensurePolling_();
}

bool SSamplerProgram::settle( int boundMs, int *waitedFor, qint64 *waitedMs, QString *why )
{
    using clock = std::chrono::steady_clock;
    // An onset analysis that FINISHED is acted on first (its re-stretch is
    // then outstanding and waited for); one still running is not waited for:
    // the barrier waits for generations, not for analyses (Q4).
    processAnalyses_();
    // A bounce the debounce is still holding back is outstanding too: start
    // it now, so the render after an asset edit hears the edit (Q3.1).
    flushDueBounces();
    const clock::time_point t0       = clock::now();
    const clock::time_point deadline = t0 + std::chrono::milliseconds( std::max( 0, boundMs ) );
    const int n = (int)jobs_.size();
    QStringList late;
    for( auto &kv : jobs_ ) {
        Job &j = *kv.second;
        std::unique_lock<std::mutex> lk( j.m );
        if( !j.cv.wait_until( lk, deadline,
                              [&j] { return j.done.load( std::memory_order_acquire ); } ) )
            late << QStringLiteral( "layer %1 generation %2" )
                        .arg( j.layerId ).arg( (qulonglong)j.generation );
    }
    installFinished_( true );
    if( waitedFor ) *waitedFor = n;
    if( waitedMs )
        *waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>( clock::now() - t0 ).count();
    ensurePolling_();
    if( !late.isEmpty() ) {
        if( why )
            *why = QStringLiteral( "%1 still rendering after %2 ms" )
                       .arg( late.join( QStringLiteral( ", " ) ) ).arg( boundMs );
        return false;
    }
    return true;
}

void SSamplerProgram::holdNextJob( std::uint32_t layerId, int holdMs )
{
    auto g    = std::make_shared<Gate>();
    g->holdMs = holdMs;
    armedGates_[layerId] = g;
}

void SSamplerProgram::releaseHeldJob( std::uint32_t layerId )
{
    armedGates_.erase( layerId );
    auto g = liveGates_.find( layerId );
    if( g == liveGates_.end() ) return;
    {
        std::lock_guard<std::mutex> lk( g->second->m );
        g->second->released = true;
    }
    g->second->cv.notify_all();
}

// --- tempo modes by stretch (Q4) ------------------------------------------------------

namespace {

double attrNum( const audio::twSamplerProgramModel &m, audio::twSamplerScope scope,
                std::uint32_t id, const char *attr, double dflt )
{
    std::string v;
    if( !m.value( scope, id, attr, &v ) || v.empty() ) return dflt;
    // QString::toDouble is the C locale, always. NOT std::strtod: the app runs
    // under the user's LC_NUMERIC (Qt calls setlocale), and under a German
    // locale strtod reads the core's "1.5" as 1 (measured: a stretch factor
    // of 0.8 for beats 1.5).
    bool ok = false;
    const double d = QString::fromStdString( v ).toDouble( &ok );
    return ok ? d : dflt;
}

}  // namespace

SSamplerProgram::Want SSamplerProgram::wantFor_( std::uint32_t layerId ) const
{
    // DESIGN §5: a layer is stretched when its zone is in a tempo mode by the
    // stretch method and its length in beats is known. Reading the zone's
    // mode and method and the layer's beats and positions is the one place
    // qbx reads program attributes beyond `src`: what to render ahead is the
    // host's decision (§5, "the host passes a buffer already rendered").
    Want w;
    std::uint32_t zone = 0;
    for( std::uint32_t z : model_.zoneIds() ) {
        const std::vector<std::uint32_t> ls = model_.layerIds( z );
        if( std::find( ls.begin(), ls.end(), layerId ) != ls.end() ) { zone = z; break; }
    }
    if( zone == 0 ) return w;
    std::string mode, method;
    model_.value( audio::twSamplerScope::Zone, zone, "mode", &mode );
    model_.value( audio::twSamplerScope::Zone, zone, "tempoMethod", &method );
    if( ( mode != "tempo-loop" && mode != "tempo-oneshot" ) || method != "stretch" ) return w;
    SProject *proj = getProjectSafe();
    const double bpm = proj ? proj->getBPMTempo() : 0.0;
    const audio::twSamplerScope L = audio::twSamplerScope::Layer;
    w.spec.bpm       = bpm;
    w.spec.beats     = attrNum( model_, L, layerId, "beats", 0.0 );
    w.spec.start     = (std::int64_t)attrNum( model_, L, layerId, "start", 0.0 );
    w.spec.end       = (std::int64_t)attrNum( model_, L, layerId, "end", -1.0 );
    w.spec.loopStart = (std::int64_t)attrNum( model_, L, layerId, "loopStart", -1.0 );
    w.spec.loopEnd   = (std::int64_t)attrNum( model_, L, layerId, "loopEnd", -1.0 );
    w.on = w.spec.beats > 0.0 && bpm > 0.0;
    return w;
}

std::shared_ptr<const std::vector<std::uint64_t>> SSamplerProgram::knownOnsets_(
    const Source &s ) const
{
    if( s.content.isNull() ) return nullptr;
    auto it = onsets_.find( { s.content.hi, s.content.lo } );
    return it != onsets_.end() ? it->second : nullptr;
}

void SSamplerProgram::checkStretch_()
{
    if( !installer_ || !isReadable() ) return;
    for( std::uint32_t z : model_.zoneIds() ) {
        for( std::uint32_t id : model_.layerIds( z ) ) {
            std::string s;
            model_.value( audio::twSamplerScope::Layer, id, "src", &s );
            if( !isFileSrc( s ) && !isLinkSrc( s ) ) continue;
            const QString src = QString::fromStdString( s );
            const Want    w   = wantFor_( id );
            auto jb = jobs_.find( id );
            const bool pendingSame = jb != jobs_.end() && jb->second->src == src;
            if( pendingSame && jb->second->want == w ) continue;   // already on its way
            auto in = installed_.find( id );
            const bool haveSame = in != installed_.end() && in->second.src == src;
            if( !pendingSame ) {
                // Nothing on its way: compare with what is installed. A
                // layer with nothing installed is reconcile's business.
                if( !haveSame ) continue;
                const Installed &I = in->second;
                bool need = I.want != w;
                if( !need && w.on && I.stretched && I.source ) {
                    // The source's onsets became known (an analysis finished)
                    // after this generation was stretched without them, or
                    // with others: re-stretch with them (DESIGN §5).
                    std::shared_ptr<const std::vector<std::uint64_t>> k = knownOnsets_( *I.source );
                    if( k && ( !I.onsets || *k != *I.onsets ) ) need = true;
                }
                if( !need ) continue;
            }
            // (Re)start: from the kept source when there is one, else from
            // the src (a decode or a bounce).
            if( haveSame && in->second.source ) {
                startJob_( id, src, in->second.source );
            } else if( isLinkSrc( s ) ) {
                LinkKey key;
                if( linkKeyOf_( src, key, nullptr ) ) startBounce_( id, src, key );
            } else {
                startJob_( id, src );
            }
        }
    }
}

void SSamplerProgram::maybeAnalyze_( std::uint32_t layerId )
{
    auto in = installed_.find( layerId );
    if( in == installed_.end() || !in->second.want.on || !in->second.source ) return;
    std::shared_ptr<const Source> source = in->second.source;
    if( knownOnsets_( *source ) ) return;   // the sidecar store had them, or an analysis did
    auto run = analysisJobs_.find( layerId );
    if( run != analysisJobs_.end() ) {
        if( run->second->source->content == source->content ) return;   // already analysing it
        run->second->cancel();
        retiredAnalyses_.push_back( run->second );
        analysisJobs_.erase( run );
        liveAnalysisGates_.erase( layerId );
    }
    auto job     = std::make_shared<AnalysisJob>();
    job->layerId = layerId;
    job->source  = source;
    auto g = armedAnalysisGates_.find( layerId );
    if( g != armedAnalysisGates_.end() ) {
        job->gate = g->second;
        liveAnalysisGates_[layerId] = g->second;
        armedAnalysisGates_.erase( g );
    }
    AnalysisJob *raw = job.get();
    job->thread = std::thread( [raw] { raw->run(); } );
    analysisJobs_[layerId] = job;
    qInfo().noquote() << QStringLiteral( "NassauDigital: layer %1: onset analysis of its source "
                                         "started%2" )
                             .arg( layerId )
                             .arg( job->gate ? QStringLiteral( " (HELD by the render gate)" )
                                             : QString() );
    ensurePolling_();
}

int SSamplerProgram::processAnalyses_()
{
    int finished = 0;
    for( auto it = analysisJobs_.begin(); it != analysisJobs_.end(); ) {
        AnalysisJob &a = *it->second;
        if( !a.done.load( std::memory_order_acquire ) ) { ++it; continue; }
        a.join();
        std::shared_ptr<const std::vector<std::uint64_t>> r;
        {
            std::lock_guard<std::mutex> lk( a.m );
            r = a.result;
        }
        if( r && !a.source->content.isNull() ) {
            onsets_[{ a.source->content.hi, a.source->content.lo }] = r;
            ++analyses_[a.layerId];
            ++finished;
            qInfo().noquote() << QStringLiteral( "NassauDigital: layer %1: onset analysis "
                                                 "finished: %2 onset(s)" )
                                     .arg( a.layerId ).arg( r->size() );
        }
        liveAnalysisGates_.erase( a.layerId );
        it = analysisJobs_.erase( it );
    }
    // What the analysis found may change a stretch layer's keyframes: the
    // re-stretch is a new generation (DESIGN §5).
    if( finished ) checkStretch_();
    ensurePolling_();
    return finished;
}

void SSamplerProgram::onTempoChanged_()
{
    checkStretch_();
}

void SSamplerProgram::holdNextAnalysis( std::uint32_t layerId, int holdMs )
{
    auto g    = std::make_shared<Gate>();
    g->holdMs = holdMs;
    armedAnalysisGates_[layerId] = g;
}

void SSamplerProgram::releaseHeldAnalysis( std::uint32_t layerId )
{
    armedAnalysisGates_.erase( layerId );
    auto g = liveAnalysisGates_.find( layerId );
    if( g == liveAnalysisGates_.end() ) return;
    {
        std::lock_guard<std::mutex> lk( g->second->m );
        g->second->released = true;
    }
    g->second->cv.notify_all();
}

bool SSamplerProgram::waitAnalyses( int boundMs, QString *why )
{
    using clock = std::chrono::steady_clock;
    const clock::time_point deadline = clock::now() + std::chrono::milliseconds( std::max( 0, boundMs ) );
    QStringList late;
    for( auto &kv : analysisJobs_ ) {
        AnalysisJob &a = *kv.second;
        std::unique_lock<std::mutex> lk( a.m );
        if( !a.cv.wait_until( lk, deadline,
                              [&a] { return a.done.load( std::memory_order_acquire ); } ) )
            late << QStringLiteral( "layer %1" ).arg( a.layerId );
    }
    processAnalyses_();
    if( !late.isEmpty() ) {
        if( why )
            *why = QStringLiteral( "onset analysis of %1 still running after %2 ms" )
                       .arg( late.join( QStringLiteral( ", " ) ) ).arg( boundMs );
        return false;
    }
    return true;
}

// --- persistence -------------------------------------------------------------------

namespace {

// The core's text inside a CDATA section. The core escapes '>' wherever it
// writes text, so "]]>" cannot occur in what it writes; a verbatim kept text
// could in principle carry one, and splitting the section is the standard way
// to keep it intact.
QString cdata( const QString &text )
{
    QString body = text;
    body.replace( QStringLiteral( "]]>" ), QStringLiteral( "]]]]><![CDATA[>" ) );
    return QStringLiteral( "<![CDATA[" ) + body + QStringLiteral( "]]>" );
}

}  // namespace

int SSamplerProgram::serialize( QTextStream &o )
{
    const char *cls = metaObject()->className();
    o << "<" << cls;
    const int res = serializeSelfAttributes( o );
    if( res < 0 ) return res;
    // THE ASSET LINKS (Q3), only those a layer names now, so a program that
    // never linked an asset is written byte-identically to Q2. Each is an
    // <SLink> child (the loader orders on those and resolves them by object
    // id); `assetLinks` says which link id each one is, by the object id the
    // <SLink> carries, and `nextLinkId` keeps ids from being reused.
    std::vector<std::pair<std::uint32_t, SLink *>> live;
    for( std::uint32_t n : namedLinks_() ) {
        auto it = links_.find( n );
        if( it != links_.end() && it->second.pin ) live.emplace_back( n, it->second.pin );
    }
    if( !live.empty() ) {
        QStringList pairs;
        for( const auto &l : live )
            pairs << QStringLiteral( "%1=%2" ).arg( l.first ).arg(
                         (qulonglong) reinterpret_cast<std::uintptr_t>( &l.second->getSObject() ) );
        o << " assetLinks='" << pairs.join( QLatin1Char( ';' ) ) << "' nextLinkId='"
          << nextLinkId_ << "'";
    }
    // THE VIEW STATE AND THE RECORD INPUT (Q6): qbx's, never the program's,
    // and each only when it is not the default, so every earlier file is
    // written byte-identically. The last key played is live and not saved.
    if( view_ != View::Key ) o << " view='" << viewText( view_ ) << "'";
    if( padBank_ != 0 ) o << " padBank='" << padBank_ << "'";
    if( pad_ != 0 ) o << " pad='" << pad_ << "'";
    if( !recordInput_.isEmpty() ) o << " recordInput='" << recordInput_.toHtmlEscaped() << "'";
    o << ">\n";
    const QString text = isReadable() ? QString::fromStdString( model_.toXml() ) : rawText_;
    o << "<program>" << cdata( text ) << "</program>\n";
    for( const auto &l : live ) l.second->serialize( o );
    o << "</" << cls << ">\n";
    return 0;
}

SLink *SSamplerProgram::instantiateFromDomElement( SProjectLoader &loader, QDomElement &element,
                                                   SObject *parent )
{
    (void) parent;
    SSamplerProgram *prog = new SSamplerProgram( &loader.getProject() );
    prog->readPreChildrenAttributes( element );

    const QDomElement pe = element.firstChildElement( QStringLiteral( "program" ) );
    const QString text   = pe.isNull() ? QString() : pe.text();
    std::string err;
    if( text.trimmed().isEmpty() ) {
        prog->loadError_ = QStringLiteral( "the element holds no program" );
    } else if( !audio::twSamplerProgramModel::fromXml( text.toStdString(), prog->model_, &err ) ) {
        prog->loadError_ = QString::fromStdString( err );
    }
    if( !prog->isReadable() ) {
        prog->rawText_ = text;
        qWarning().noquote()
            << QStringLiteral( "NassauDigital: the program stored in the project cannot be read "
                               "(%1); it is KEPT VERBATIM and written back on save, the "
                               "instrument plays nothing, and edits are refused" )
                   .arg( prog->loadError_ );
    }

    // THE ASSET LINKS (Q3). The loader built this element only once every
    // <SLink> child resolved; one whose body the file lost was pruned (a
    // CONTAINER kind, below) and its layer plays nothing, announced.
    std::map<QString, QList<std::uint32_t>> idsByObject;
    std::uint32_t maxId = 0;
    for( const QString &pair : element.attribute( QStringLiteral( "assetLinks" ) )
                                   .split( QLatin1Char( ';' ), Qt::SkipEmptyParts ) ) {
        const int eq = pair.indexOf( QLatin1Char( '=' ) );
        if( eq <= 0 ) continue;
        const std::uint32_t id = (std::uint32_t)pair.left( eq ).toULongLong();
        if( id == 0 ) continue;
        idsByObject[pair.mid( eq + 1 )].append( id );
        maxId = std::max( maxId, id );
    }
    for( QDomElement c = element.firstChildElement( QStringLiteral( "SLink" ) ); !c.isNull();
         c = c.nextSiblingElement( QStringLiteral( "SLink" ) ) ) {
        const QString oid = c.attribute( QStringLiteral( "objectId" ) );
        SLink *target     = loader.getObjectDictionary().value( oid );
        auto ids          = idsByObject.find( oid );
        if( !target || ids == idsByObject.end() || ids->second.isEmpty() ) {
            qWarning().noquote() << QStringLiteral( "NassauDigital: an asset link (object %1) "
                                                    "cannot be restored; skipped" ).arg( oid );
            continue;
        }
        const std::uint32_t id = ids->second.takeFirst();
        LinkRec &r = prog->links_[id];
        r.body = &target->getSObject();
        r.pin  = new SLink( target->getSObject(), prog );
    }
    prog->nextLinkId_ = std::max<std::uint32_t>(
        maxId + 1, (std::uint32_t)element.attribute( QStringLiteral( "nextLinkId" ), "1" ).toULongLong() );

    // THE VIEW STATE AND THE RECORD INPUT (Q6). A value this build cannot
    // take (a view a newer build added, a pad out of range) is announced and
    // the default kept.
    {
        View v = View::Key;
        const QString vt = element.attribute( QStringLiteral( "view" ), QStringLiteral( "key" ) );
        if( !viewFromText( vt, v ) ) {
            qWarning().noquote() << QStringLiteral( "NassauDigital: unknown view \"%1\"; the Key "
                                                    "view is used" ).arg( vt );
            v = View::Key;
        }
        const int bank = element.attribute( QStringLiteral( "padBank" ), "0" ).toInt();
        const int pad  = element.attribute( QStringLiteral( "pad" ), "0" ).toInt();
        if( prog->padExists( bank, pad ) ) {
            prog->view_    = v;
            prog->padBank_ = bank;
            prog->pad_     = pad;
        } else {
            qWarning().noquote() << QStringLiteral( "NassauDigital: pad %1 of bank %2 does not "
                                                    "exist; pad 0 of bank 0 is used" )
                                        .arg( pad ).arg( bank );
            prog->view_ = v;
        }
        const QString ri = element.attribute( QStringLiteral( "recordInput" ) );
        if( isValidRecordInput( ri ) ) prog->recordInput_ = ri;
        else
            qWarning().noquote() << QStringLiteral( "NassauDigital: record input \"%1\" is not "
                                                    "audio:<device>:<mask>; none is used" ).arg( ri );
    }

    prog->readPostChildrenAttributes( element );
    return new SLink( *prog );
}

// Self-registration with the project loader (proposal 14, Phase 5), as
// SPluginSlot does: legal because the app is an OBJECT library.
static const bool s_registered_ssamplerprogram =
    ( SProjectLoader::registerSObjectClass( "SSamplerProgram",
                                            SSamplerProgram::instantiateFromDomElement,
                                            // A CONTAINER since Q3, when a program
                                            // gained asset links: a body the file
                                            // lost costs that link, never the
                                            // user's program.
                                            SElementKind::Container ),
      true );
