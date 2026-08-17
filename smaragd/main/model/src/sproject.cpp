
#include <qobject.h>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdint>
#include <cstdlib>

using namespace std;

#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/model/sexternfile.h"
#include "tw/schedule/capture_revalidator.h"

// Minimal XML escaping for embedding a JSON string inside a single-quoted
// attribute. JSON's own double quotes are fine inside single quotes; we only
// need to guard the characters that would break the attribute or the markup.
static QString xmlAttrEscape( const QString &s )
{
    QString out = s;
    out.replace( '&', "&amp;" );
    out.replace( '<', "&lt;" );
    out.replace( '\'', "&apos;" );
    return out;
}

#include "app/model/slink.h"


QString QuoteString( const QString &org )
{
    return org;
}

QString UnquoteString( const QString &org )
{
    return org;
}

int SProject::serialize( QTextStream &o )
{
    o << "<SProject fileName='"+fileName_+"' rootId='";
    // As the link to he current root component is not my child, I
    // have to serialize it explicitely.
    if( soRoot_ ) {
        o << reinterpret_cast<std::uintptr_t>(&(soRoot_->getSObject()));
    } else {
        o << "0";
    }
    o << "'";
    serializeSelfAttributes( o );
    o << ">\n";
    const QObjectList& children = this->children();
    for (QObjectList::const_iterator it = children.begin(); it != children.end(); ++it ) {
        SObject* so = (SObject*) *it;        
        int res = so->serialize( o );
        if( res<0 ) break;
    }
    o << "</SProject>";
    return 0;
}

int SProject::serializeSelfAttributes( QTextStream &o )
{
    // Wire-format version (proposal 37 D8a). Written unconditionally, read
    // with a default of 1, never a reason to refuse a document — see
    // SProject::FORMAT_VERSION.
    o << " formatVersion='" << FORMAT_VERSION << "'";
    o << " bpmTempo='" << (double) getBPMTempo() << "'";
    o << " sampleRate='" << sampleRate_ << "'";
    o << " channels='" << channels_ << "'";
    o << " posFactor='" << QString::fromStdString( posFactor_.toString() ) << "'";
    o << " candidateRates='";
    for( size_t i = 0; i < candidateRates_.size(); ++i ) {
        if( i ) o << ",";
        o << candidateRates_[i];
    }
    o << "'";

    // The generic property dict, serialized as JSON in a single attribute.
    QJsonObject obj = QJsonObject::fromVariantMap( properties_ );
    QByteArray json = QJsonDocument( obj ).toJson( QJsonDocument::Compact );
    o << " properties='" << xmlAttrEscape( QString::fromUtf8( json ) ) << "'";
    return 0;
}

int SProject::readPreChildrenAttributes( QDomElement &element )
{
    QString data;

    // Wire-format version. Absent = 1 (every project written before proposal
    // 36). A HIGHER version is warned about and then read anyway: the document
    // is still XML we know how to walk, an element we do not know is already
    // skipped by name, and refusing would strand a user's file on the build
    // they happen to have installed.
    bool fvOk = false;
    const int fv = element.attribute( "formatVersion", "1" ).toInt( &fvOk );
    formatVersion_ = ( fvOk && fv > 0 ) ? fv : 1;
    if( formatVersion_ > FORMAT_VERSION ) {
        qWarning() << QString( "Project declares formatVersion '%1', newer than "
                               "this build understands (%2); loading it anyway — "
                               "anything it describes that we do not know will be "
                               "skipped with its own warning." )
                          .arg( formatVersion_ ).arg( (int) FORMAT_VERSION );
    }

    data = element.attribute( "bpmTempo", "120.0" );
    setBPMTempo( data.toDouble() );

    // Load sample rate from file; default to 44100 for legacy pre-sample-rate files.
    // WARNING: If this file was saved with a different sample rate and the attribute
    // is missing/corrupted, all timeline positions will be incorrect (scaled by the
    // ratio of saved-rate to loaded-rate).
    QString srateStr = element.attribute( "sampleRate" );
    if( srateStr.isEmpty() ) {
        qWarning() << "WARNING: Project file missing sampleRate attribute. "
                   << "Defaulting to 44100 Hz. If this file was created with a different "
                   << "sample rate, timeline positions will be misaligned. Save the project "
                   << "to correct this.";
        setSRate( 44100 );
    } else {
        setSRate( srateStr.toInt() );
    }

    // Channel count — the sampleRate idiom above, deliberately copied.
    //
    // A missing attribute means a file written before proposal 36 M1, and every
    // such file sounds like 2 channels, so 2 is the only defensible default.
    // It WARNS rather than defaulting silently for the same reason the rate
    // does: a wrong channel count is as corrupting as a wrong rate — it would
    // silently narrow or widen the project's output the day the signal flow
    // goes wide (B5). Exactly one warning per load, whichever way it fails.
    //
    // A present-but-invalid value (a typo, a hand-edit, a width a future build
    // wrote and this one does not know) also falls back to 2 rather than
    // refusing the document: a project must never fail to open over this.
    QString chStr = element.attribute( "channels" );
    if( chStr.isEmpty() ) {
        qWarning() << "WARNING: Project file missing channels attribute. "
                   << "Defaulting to 2 channels (stereo), which is what every "
                   << "project written before this attribute existed sounds "
                   << "like. Save the project to correct this.";
        setChannels( 2 );
    } else {
        bool ok = false;
        int ch = chStr.toInt( &ok );
        if( !ok || !isValidChannelCount( ch ) ) {
            qWarning() << "WARNING: Project file has an unsupported channels "
                       << "attribute" << chStr
                       << "- defaulting to 2. Supported: 1, 2, 4, 6, 8.";
            setChannels( 2 );
        } else {
            setChannels( ch );
        }
    }

    // Load position factor (time coordinate scaling). Default to 1/sampleRate.
    QString posFactorStr = element.attribute( "posFactor" );
    if( !posFactorStr.isEmpty() ) {
        posFactor_ = parseFractionOrDouble( posFactorStr.toStdString() );
    }
    // If not present in file, posFactor was already set to 1/sampleRate in setSRate()

    QString cr = element.attribute( "candidateRates", "44100,48000,88200,96000" );
    std::vector<std::uint32_t> rates;
    const QStringList parts = cr.split( ',', Qt::SkipEmptyParts );
    for( const QString &p : parts ) {
        bool ok = false;
        unsigned int v = p.trimmed().toUInt( &ok );
        if( ok && v > 0 ) rates.push_back( v );
    }
    if( !rates.empty() ) setCandidateRates( std::move( rates ) );

    // Generic property dict (JSON). Merge over the seeded defaults so missing
    // keys keep their default and unknown (future) keys are preserved.
    const QString pj = element.attribute( "properties" );
    if( !pj.isEmpty() ) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson( pj.toUtf8(), &err );
        if( err.error == QJsonParseError::NoError && doc.isObject() ) {
            const QVariantMap loaded = doc.object().toVariantMap();
            for( auto it = loaded.cbegin(); it != loaded.cend(); ++it ) {
                properties_.insert( it.key(), it.value() );
            }
        } else {
            qWarning() << "SProject: ignoring malformed properties JSON:" << err.errorString();
        }
    }
    return 0;
}

QVariant SProject::prop( const QString &key, const QVariant &defaultValue ) const
{
    return properties_.value( key, defaultValue );
}

void SProject::setProp( const QString &key, const QVariant &value )
{
    if( properties_.value( key ) == value ) return;   // no-op if unchanged
    properties_.insert( key, value );
    emit propertyChanged( key, value );
}

bool SProject::hasProp( const QString &key ) const
{
    return properties_.contains( key );
}

SObject *SProject::getRootComponent() const
{
    if( soRoot_ ) {
        return &(soRoot_->getSObject());
    } else {
        return NULL;
    }
}

void SProject::setRootComponent( SObject *obj )
{
    // No change?
    if( (soRoot_ && (obj==&(soRoot_->getSObject())))
        || (!soRoot_ && obj==NULL) ) return;
    // FIXME: Trigger views.
    if( soRoot_ ) {
        // Dereference old one.
        delete soRoot_;
        soRoot_ = NULL;
    }
    if( obj ) {
        soRoot_ = new SLink( *obj, NULL );
    }
}

void SProject::setTempoMap( const twTempoMap &map )
{
    tempoMap_ = map;
    // One signal for every tempo-derived view. bpmTempoChanged is what the
    // ruler, the transport box and SMidiCut already listen to, and BPM is the
    // map's derived view — a second signal would only let the two drift.
    emit bpmTempoChanged( tempoMap_.bpm() );
}

void SProject::setBPMTempo( double newTempo )
{
    // BPM is a VIEW of the map (D2): this stores round(6e7/bpm) µs per quarter
    // and everything reads back through the map. Callers are the `set-tempo`
    // verb and the loader; a direct call from a widget is the bug this
    // proposal removed (the ruler dialog and the transport box both did it).
    twTempoMap map = tempoMap_;
    map.setBpm( newTempo );
    setTempoMap( map );
}

void SProject::setSRate( int rate )
{
    if( rate <= 0 ) return;
    sampleRate_ = rate;
    // Initialize posFactor to 1/sampleRate (default: positions in sample units)
    posFactor_ = Fraction( 1, rate );
    emit sampleRateChanged( rate );
}

bool SProject::isValidChannelCount( int n )
{
    // 1 / 2 / 4 / 6 / 8 — the widths proposal 36 delivers. Not "any n >= 1":
    // every one of these has to be a real, gated configuration by B5, and an
    // arbitrary width would be a promise nothing keeps.
    return n == 1 || n == 2 || n == 4 || n == 6 || n == 8;
}

void SProject::setChannels( int n )
{
    if( !isValidChannelCount( n ) ) {
        qWarning() << "SProject::setChannels: refusing unsupported channel count"
                   << n << "- supported: 1, 2, 4, 6, 8.";
        return;
    }
    if( n == channels_ ) return;
    channels_ = n;
    // DELIBERATELY the end of the line (proposal 36 M1): nobody connects this
    // to a bus count yet. The signal exists so B4/B5 has one seam to widen,
    // not because anything downstream is listening today.
    emit channelsChanged( n );
}

void SProject::setCandidateRates( std::vector<std::uint32_t> rates )
{
    if( rates.empty() ) return;
    candidateRates_ = std::move( rates );
}

length_t SProject::getDurationFrames() const
{
    // ONE notion of "how long is this project": the root container's content
    // extent, which the model already maintains and the arranger already draws
    // (SStdMixer::getDuration() -> SObject::getChildrenExtent(), relayed to
    // SStdMixerView::contentDurationChanged). No second traversal — a duration
    // that disagreed with the one on screen would be worse than the old
    // hardcoded constant.
    //
    // getChildrenExtent() reports the LAST END, not end-minus-first-start, so
    // this is the arrangement end measured from position 0 — exactly what a
    // whole-project render needs.
    SObject *root = getRootComponent();
    if( !root ) return 0;
    length_t frames = root->getDuration();
    // An EMPTY container reports 1, not 0: both SStdMixer::getDuration() and
    // STrack::getDuration() floor their extent at 1 sample. Treat that sentinel
    // as what it means — no content.
    if( frames <= 1 ) return 0;
    return frames;
}

double SProject::getDurationSeconds() const
{
    const int rate = getSRate();
    if( rate <= 0 ) return 0.0;
    return (double) getDurationFrames() / (double) rate;
}

bool SProject::hasTimeSelection() const
{
    return prop(SProjectProps::RangeValid, false).toBool();
}

SProject::TimeRange SProject::getTimeSelection() const
{
    // Read from the UI's time-range marker (stored in samples)
    bool rangeValid = prop(SProjectProps::RangeValid, false).toBool();
    if (rangeValid) {
        // Convert from samples to seconds using project sample rate
        double sampleRate = getSRate();
        double startSec = prop(SProjectProps::RangeStart, 0).toULongLong() / sampleRate;
        double endSec = prop(SProjectProps::RangeEnd, 0).toULongLong() / sampleRate;
        return { startSec, endSec };
    }
    return { 0.0, getDurationSeconds() };
}

void SProject::setTimeSelection(double startSeconds, double endSeconds)
{
    // Store in the UI's time-range marker (convert from seconds to samples)
    double sampleRate = getSRate();
    setProp(SProjectProps::RangeValid, true);
    setProp(SProjectProps::RangeStart, (qulonglong)(startSeconds * sampleRate));
    setProp(SProjectProps::RangeEnd, (qulonglong)(endSeconds * sampleRate));
}

void SProject::clearTimeSelection()
{
    properties_.remove("render/timeSelection_in");
    properties_.remove("render/timeSelection_out");
}

void SProject::setFileName( const QString &fileName )
{
    fileName_ = fileName;
    emit fileNameChanged( fileName_ );
}

void SProject::setProjectFilePath( const QString &path )
{
    // Absolutized once, here, so every consumer gets the same anchor no matter
    // what the working directory was when the save/load was requested.
    projectFilePath_ = path.isEmpty()
        ? QString()
        : QDir::cleanPath( QFileInfo( path ).absoluteFilePath() );
}

void SProject::addExternObject( const SExternFile &extObject )
{
    QString externFileName = extObject.getFileName();
    qWarning() << QString( "Name of extern file is \"%1\"." ).arg( externFileName );
    externFileDict_.insert( externFileName, const_cast<SExternFile*>( &extObject ) );
    emit externFileAdded( extObject );
}

void SProject::removeExternObject( QString &fileName )
{
    // Guard against being called during destruction of a partial load
    // when this method might be called from a child's destructor
    if( !fileName.isEmpty() && externFileDict_.contains( fileName ) ) {
        externFileDict_.remove( fileName );
        emit externFileRemoved( fileName );
    }
}

void SProject::registerAsset( const QString &name, SObject *body )
{
    if( !body || name.isEmpty() ) return;
    if( assetDict_.contains( name ) ) {
        qWarning() << QString( "SProject::registerAsset: name already in use: \"%1\"." )
                          .arg( name );
        return;
    }
    assetDict_.insert( name, body );
    body->addRef();                  // pin: the asset survives with zero placements
    emit assetAdded( name, *body );
}

void SProject::unregisterAsset( const QString &name )
{
    SObject *body = assetDict_.take( name );
    if( !body ) return;
    emit assetRemoved( name );       // let listeners drop their row before the body dies
    body->removeRef();               // may bring the refcount to 0 -> deleteLater
}

SObject *SProject::asset( const QString &name ) const
{
    return assetDict_.value( name );
}

bool SProject::hasAsset( const QString &name ) const
{
    return assetDict_.contains( name );
}

QString SProject::assetNameOf( const SObject *body ) const
{
    if( !body ) return QString();
    for( auto it = assetDict_.constBegin(); it != assetDict_.constEnd(); ++it ) {
        if( it.value() == body ) return it.key();
    }
    return QString();
}

QList<QString> SProject::assetNames() const
{
    return assetDict_.keys();
}

static SProject::ExternFileFactory &externFileFactory()
{
    static SProject::ExternFileFactory factory = nullptr;
    return factory;
}

void SProject::registerExternFileFactory( ExternFileFactory f )
{
    externFileFactory() = f;
}

static QHash<QString, SProject::ContentFileFactory> &contentFileFactories()
{
    static QHash<QString, SProject::ContentFileFactory> factories;
    return factories;
}

void SProject::registerContentFileFactory( const QStringList &suffixes,
                                           ContentFileFactory f )
{
    if( !f ) return;
    for( const QString &sfx : suffixes )
        contentFileFactories().insert( sfx.toLower(), f );
}

QStringList SProject::contentFileSuffixes()
{
    QStringList out = contentFileFactories().keys();
    out.sort();
    return out;
}

SLink *SProject::linkToFile( QString &fileName )
{
    // Resolve a relative path against the action-script directory when one is
    // set and the file actually exists there. This makes a .qxa's
    // "../foo.wav" resolve next to the script regardless of the working
    // directory — otherwise a test run from the wrong CWD silently picks up a
    // stale same-named file elsewhere (see proposal 19 Phase 0). Falls back to
    // the path as given (CWD-relative) when unset or not found, so GUI use is
    // unchanged.
    if( !sampleBaseDir_.isEmpty() && QFileInfo( fileName ).isRelative() ) {
        const QString resolved =
            QDir::cleanPath( QDir( sampleBaseDir_ ).filePath( fileName ) );
        if( QFileInfo::exists( resolved ) ) {
            fileName = resolved;
        }
    }

    // Suffix-claimed content (a .mid) first: it is materialised inline and has
    // no extern-file identity to cache (see registerContentFileFactory).
    const QString suffix = QFileInfo( fileName ).suffix().toLower();
    if( ContentFileFactory cf = contentFileFactories().value( suffix, nullptr ) ) {
        SObject *obj = cf( this, fileName );
        if( !obj ) return NULL;
        return new SLink( *obj );
    }

    SExternFile *ef = externFileDict_.value( fileName );
    if( !ef ) {
        ExternFileFactory factory = externFileFactory();
        if( !factory ) return NULL;
        ef = factory( this, fileName );
        if( !ef ) return NULL;
    }
    return new SLink( *ef );
}

SProject::~SProject()
{
    // For partially-loaded projects that failed during object creation, skip
    // the normal cleanup since accessing the partially-constructed objects
    // could crash. Qt's parent-child mechanism will still clean them up.
    if( isPartialLoad_ ) {
        // Quiesce FIRST. Returning early hands the object graph to ~QObject,
        // which frees the children in its own order while the revalidator's
        // workers may still hold raw pointers into them. pauseRevalidation()
        // blocks until in-flight jobs drain, so after this line no worker is
        // inside an object that is about to be deleted. (The revalidator itself
        // is a member and outlives this body — it dies after the destructor
        // returns, before ~QObject runs.)
        pauseRevalidation();
        return;
    }

    DTOR_DEL( soRoot_ );   // drop the root reference (parent==NULL, not a child)

    // Release the pins registerAsset() took. They are the PROJECT's own
    // references: held here they make every asset body — and its whole
    // subtree — look permanently referenced, so the cascade below skips it and
    // the survivor pass has to hard-delete it while live SLinks still point at
    // it. Drop them first and the normal cascade collects assets like anything
    // else. (No assetRemoved signal: the views are already detached by now.)
    {
        const QList<SObject*> bodies = assetDict_.values();
        assetDict_.clear();
        for( SObject *body : bodies ) {
            if( body ) body->removeRef();
        }
    }

    // Tear down the object graph here, in the destructor body, while our members
    // (externFileDict_, ...) are still alive — child destructors call back into
    // the project (e.g. SPlainWave deregisters itself from externFileDict_).
    //
    // Every SObject is both a QObject child of the project AND reference-counted
    // via SLinks. Delete only objects whose reference count has reached zero,
    // repeatedly: deleting an object frees its child SLinks, which drop the
    // references they held, bringing the next layer to zero. Starting from the
    // root (whose reference we just dropped) this cascades root -> leaf, so no
    // SLink ever dereferences an already-freed object.
    bool progress = true;
    while( progress ) {
        progress = false;
        const QObjectList kids = children();   // copy: mutates as we delete
        for( QObject *kid : kids ) {
            // Use dynamic_cast to safely check if child is an SObject.
            // Non-SObject QObjects (like DSP components) are skipped here
            // and handled by Qt's parent-child cleanup below.
            SObject *so = dynamic_cast<SObject*>( kid );
            if( so && so->getNReferences() == 0 ) {
                // Explicit lifecycle control: remove from parent first, then delete.
                // This makes the parent-child relationship state transition explicit.
                // Destructors that call getProjectSafe() will safely get nullptr,
                // avoiding any implicit behavior during destruction.
                so->setParent( nullptr );
                delete so;
                progress = true;
            }
        }
    }

    // Survivors: objects the cascade could not reach because something outside
    // the project still references them (an undo-stack action's pin is the
    // usual case) or because they form a cycle. Normally this list is empty.
    //
    // Deleting them outright — what this used to do — is what produced the
    // "destroyed with N live reference(s)" crash: every SLink still pointing at
    // the freed object dangled, and the next ~SLink ran removeRef() on freed
    // memory. So delete referrers BEFORE referents: repeatedly take the
    // survivors that no other survivor links to. That order guarantees each
    // object dies only once nothing inside the project can still reach it.
    QList<SObject*> remaining;
    for( QObject *kid : children() ) {
        if( SObject *so = dynamic_cast<SObject*>( kid ) ) remaining.append( so );
    }
    while( !remaining.isEmpty() ) {
        QHash<SObject*,int> inDegree;
        for( SObject *so : remaining ) inDegree.insert( so, 0 );
        auto countEdge = [&inDegree]( SLink *lk ) {
            if( !lk ) return;
            auto it = inDegree.find( &lk->getSObject() );
            if( it != inDegree.end() ) ++it.value();
        };
        for( SObject *so : remaining ) {
            for( SLink *lk : so->childLinks() )     countEdge( lk );
            // Owned-but-not-child links are edges too, and missing one is not a
            // near miss: STrack's reference to its SPluginChain is invisible to
            // childLinks(), so this pass used to put the chain in the SAME batch
            // as its track, delete the chain first, and leave ~STrack's
            // `delete cpPluginChainRef_` calling removeRef() on freed memory —
            // the teardown SEGFAULT after a passing headless run.
            for( SLink *lk : so->ownedRefLinks() ) countEdge( lk );
        }
        QList<SObject*> batch;
        for( SObject *so : remaining ) {
            if( inDegree.value( so )==0 ) batch.append( so );
        }
        if( batch.isEmpty() ) {
            // A cycle among the survivors: no safe order exists. Take the whole
            // rest — the per-object warnings below still name what dangled.
            batch = remaining;
        }
        for( SObject *so : batch ) {
            if( so->getNReferences() > 0 ) {
                qWarning( "SProject::~SProject(): '%s' outlived the refcount "
                          "cascade with %d reference(s) held outside the "
                          "project.",
                          qPrintable( so->getSName() ), so->getNReferences() );
            }
            so->setParent( nullptr );
            delete so;
            remaining.removeOne( so );
        }
    }

    // Non-SObject children (DSP components and friends) are Qt's to clean up.
}

void SProject::markAsPartialLoad()
{
    // Mark project as partially-loaded so destructor skips unsafe cleanup.
    // Qt's parent-child mechanism will still clean up children safely.
    isPartialLoad_ = true;
}

SProject::SProject()
    : soRoot_( NULL ),
      sampleRate_( 48000 ),
      channels_( 2 ),
      posFactor_( 1, 48000 ),
      candidateRates_{ 44100, 48000, 88200, 96000 },
      properties_( SProjectProps::defaults() )
{
    // Initialize async capture revalidation infrastructure (Phase 4).
    // Pre-allocate page pool: 2048 pages × 256kB = 512MB per project.
    // This supports ~100-500 concurrent cut revalidations with headroom.
    pagePool_ = std::make_unique<CapturePagePool>(2048);

    // Spawn worker thread pool: 8 threads for aggressive race condition testing.
    // In production, this can be dialed back to 2-4 (diminishing returns above that).
    //
    // SMARAGD_REVAL_WORKERS overrides the count (clamped to [1,64]) — the
    // determinism gates sweep it (repeat_test.sh's 4th argument; proposal 19).
    // "0" disables background revalidation entirely (no revalidator): the
    // decisive is-it-a-worker-race diagnostic from the proposal-19 hunts.
    int numWorkers = 8;
    bool noReval = false;
    if (const char *env = getenv("SMARAGD_REVAL_WORKERS")) {
        int n = atoi(env);
        if (n == 0)      noReval = true;
        else if (n > 0)  numWorkers = n < 64 ? n : 64;
    }
    if (!noReval)
        revalidator_ = std::make_unique<CaptureRevalidator>(pagePool_.get(), numWorkers);

#if 0
    soRoot_ = new SStdMixer( this );
#endif
}

void SProject::pauseRevalidation()
{
    if (revalidator_) revalidator_->pause();
}

void SProject::resumeRevalidation()
{
    if (revalidator_) revalidator_->resume();
}

void SProject::enableInvalidation()
{
    if (invalidationSuppressed_ > 0) {
        invalidationSuppressed_--;
    }

    // If we just disabled the last suppression, trigger full revalidation pass.
    // All cuts that were loaded need their captures recomputed.
    if (invalidationSuppressed_ == 0) {
        // Signal views to refresh: grouped assets, timelines, etc. all need to
        // repaint as captures transition from empty to revalidated state.
        // Individual cuts will be invalidated on first access (lazy model),
        // but views need to know to trigger repaints as data becomes available.
        emit arrangementChanged();
    }
}
