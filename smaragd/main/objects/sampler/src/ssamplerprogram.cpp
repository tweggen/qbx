#include "app/objects/sampler/ssamplerprogram.h"

#include "app/model/sappcontext.h"
#include "app/model/sfilepathref.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/persistence/sprojectloader.h"

#include "tw/plugins/twplugin.h"
#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplertypes.h"
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
#include <condition_variable>
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

struct SSamplerProgram::Job {
    std::uint32_t layerId    = 0;
    std::uint64_t generation = 0;
    QString       src;             // main thread only
    std::string   path;            // the resolved file, read by the worker
    tw303aEnvironment *env = nullptr;
    std::shared_ptr<Gate> gate;     // set-sampler-render-gate, or null

    std::mutex              m;
    std::condition_variable cv;
    std::atomic<bool>       done{ false };
    std::atomic<bool>       cancelled{ false };
    std::shared_ptr<const audio::twSamplerSample> result;   // under m, once done
    std::string             error;                          // under m, once done
    std::chrono::steady_clock::time_point started;
    std::thread             thread;

    void run()
    {
        if( gate ) {
            std::unique_lock<std::mutex> lk( gate->m );
            auto released = [this] {
                return gate->released || cancelled.load( std::memory_order_acquire );
            };
            if( gate->holdMs >= 0 )
                gate->cv.wait_for( lk, std::chrono::milliseconds( gate->holdMs ), released );
            else
                gate->cv.wait( lk, released );
        }
        std::shared_ptr<const audio::twSamplerSample> out;
        std::string err;
        if( cancelled.load( std::memory_order_acquire ) ) {
            err = "cancelled";
        } else if( !env ) {
            err = "no audio environment to decode with";
        } else {
            twSampleSource src( *env, QString::fromStdString( path ) );
            const int ch = src.wasLoaded() ? (int)src.channels() : 0;
            if( !src.wasLoaded() ) {
                err = "cannot decode " + path;
            } else if( ch != 1 && ch != 2 ) {
                err = path + " has " + std::to_string( ch ) + " channels; a sample has 1 or 2";
            } else {
                const float *planar[2] = { src.channelData( 0 ),
                                           ch > 1 ? src.channelData( 1 ) : nullptr };
                out = audio::twSamplerSample::fromPlanar( ch, (double)src.sampleRate(), planar,
                                                         (std::int64_t)src.length(),
                                                         generation, &err );
            }
        }
        {
            std::lock_guard<std::mutex> lk( m );
            result = std::move( out );
            error  = std::move( err );
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
}

SSamplerProgram::~SSamplerProgram()
{
    // Every worker is joined here, on the main thread: a held one is released
    // by cancel(), a decoding one finishes its file. No worker outlives the
    // object it reports to (it reports to nothing, but its Job is shared).
    for( auto &kv : jobs_ ) kv.second->cancel();
    for( auto &j : retiredJobs_ ) j->cancel();
    for( auto &kv : jobs_ ) kv.second->join();
    for( auto &j : retiredJobs_ ) j->join();
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
        reconcileSamples_();
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
    }
    auto jb = jobs_.find( layerId );
    if( jb != jobs_.end() ) {
        st.pendingGeneration = jb->second->generation;
        st.pendingSrc        = jb->second->src;
    }
    auto n = installs_.find( layerId );
    if( n != installs_.end() ) st.installs = n->second;
    return st;
}

int SSamplerProgram::outstandingGenerations() const
{
    return (int)jobs_.size();
}

void SSamplerProgram::reconcileSamples_()
{
    // Nothing to feed while unbound: bind (setInstaller) reconciles.
    if( !installer_ ) return;

    std::set<std::uint32_t> layers;
    std::vector<std::uint32_t> removed;
    for( std::uint32_t z : model_.zoneIds() ) {
        for( std::uint32_t id : model_.layerIds( z ) ) {
            layers.insert( id );
            std::string src;
            model_.value( audio::twSamplerScope::Layer, id, "src", &src );
            const QString want = isFileSrc( src ) ? QString::fromStdString( src ) : QString();

            auto jb = jobs_.find( id );
            if( jb != jobs_.end() ) {
                if( jb->second->src == want ) continue;   // already on its way
                cancelJob_( id );                         // superseded
            }
            auto in = installed_.find( id );
            const QString have = in != installed_.end() ? in->second.src : QString();
            if( want == have ) continue;
            if( want.isEmpty() ) {
                installed_.erase( id );
                removed.push_back( id );
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

    if( !removed.empty() ) installChanged_( removed );
}

void SSamplerProgram::startJob_( std::uint32_t layerId, const QString &src )
{
    auto job        = std::make_shared<Job>();
    job->layerId    = layerId;
    job->generation = ++lastGeneration_[layerId];
    job->src        = src;
    job->path       = resolveFileSrc( getProjectSafe(), src ).toStdString();
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
    qInfo().noquote() << QStringLiteral( "NassauDigital: layer %1: generation %2 rendering from "
                                         "%3%4" )
                             .arg( layerId ).arg( (qulonglong)job->generation )
                             .arg( QString::fromStdString( job->path ) )
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
        {
            std::lock_guard<std::mutex> lk( j.m );
            sample = j.result;
            err    = j.error;
        }
        if( sample ) {
            installed_[j.layerId] = Installed{ j.generation, j.src, sample };
            ++installs_[j.layerId];
            changed.push_back( j.layerId );
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
    if( jobs_.empty() && retiredJobs_.empty() ) {
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
    installFinished_( false );
    ensurePolling_();
}

bool SSamplerProgram::settle( int boundMs, int *waitedFor, qint64 *waitedMs, QString *why )
{
    using clock = std::chrono::steady_clock;
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
    o << ">\n";
    const QString text = isReadable() ? QString::fromStdString( model_.toXml() ) : rawText_;
    o << "<program>" << cdata( text ) << "</program>\n";
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

    prog->readPostChildrenAttributes( element );
    return new SLink( *prog );
}

// Self-registration with the project loader (proposal 14, Phase 5), as
// SPluginSlot does: legal because the app is an OBJECT library.
static const bool s_registered_ssamplerprogram =
    ( SProjectLoader::registerSObjectClass( "SSamplerProgram",
                                            SSamplerProgram::instantiateFromDomElement ),
      true );
