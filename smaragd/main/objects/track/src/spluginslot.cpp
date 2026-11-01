#include "app/objects/track/spluginslot.h"
#include "app/model/sautomationlane.h"
#include "app/model/sproject.h"
#include "app/model/slink.h"
#include "app/model/sappcontext.h"
#include "app/persistence/sprojectloader.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/graph/twlatch.h"
#include "tw/core/twlog.h"
#include <algorithm>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDomElement>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTextStream>

namespace {

// The project file writes attributes in SINGLE quotes and the rest of the model
// escapes nothing at all — which is fine for numbers, and a corrupt file the
// moment a plugin is called "Bob's & Co <EQ>". Escape exactly the characters
// that would break the attribute or the markup; the value round-trips through
// QDomElement::attribute() on the way back in.
QString attrEscape( const QString &s )
{
    QString out = s;
    out.replace( '&', "&amp;" );
    out.replace( '<', "&lt;" );
    out.replace( '>', "&gt;" );
    out.replace( '\'', "&apos;" );
    return out;
}

QString attrEscape( const std::string &s )
{
    return attrEscape( QString::fromStdString( s ) );
}

}  // namespace

SPluginSlot::SPluginSlot( SProject *project, const audio::twPluginDescriptor &desc )
    : SObject( project ), descriptor_( desc ), effective_( desc )
{
    setSName( QString::fromStdString( desc.name ) );

    resolveEffective();

    // The processor and its insert are created on demand (setChannelCount / the
    // first getInsert), because the channel count is what decides the
    // channel-mismatch mapping and only STrack knows it.
}

SPluginSlot::~SPluginSlot()
{
    // FIRST, before any member -- proc_ (and its live audio::twPlugin
    // instance) included -- is torn down. See spluginslot.h's own comment on
    // slotDestroying() for the crash this fixes: a listener closing a native
    // or generic plugin editor here still finds a live plugin to detach()
    // from, where the base QObject's own destroyed() signal (fired from
    // ~QObject(), which runs strictly after this body and every member
    // destructor) would not.
    emit slotDestroying();
}

// Resolve (format, uid) against the registry: a scanned record carries the
// module PATH and the plugin's REAL channel counts (the scanner instantiated it
// to read them), both of which a hand-built or older-project descriptor may be
// missing or wrong about. Falls back to what we were given — with its module
// path resolved — which is what keeps a headless run without a scan working.
//
// descriptor_ itself is NEVER rewritten here. It is what the project file said
// and what the project file will say again (proposal 08 M4): resolving a
// relative path into it would turn a portable project into a machine-specific
// one on the first save.
void SPluginSlot::resolveEffective()
{
    audio::twPluginDescriptor resolved;
    if( audio::pluginRegistry().findByUid( descriptor_.format, descriptor_.uid,
                                          resolved ) ) {
        effective_ = resolved;
        return;
    }
    effective_ = descriptor_;
    effective_.path = resolveModulePath( getProjectSafe(),
                                         QString::fromStdString( descriptor_.path ) )
                          .toStdString();
}

QString SPluginSlot::resolveModulePath( SProject *project, const QString &path )
{
    if( path.isEmpty() ) return path;
    QFileInfo fi( path );
    if( fi.isAbsolute() ) return QDir::cleanPath( path );

    if( project ) {
        const QString base = project->sampleBaseDir();
        if( !base.isEmpty() ) {
            const QString cand = QDir::cleanPath( QDir( base ).filePath( path ) );
            if( QFileInfo::exists( cand ) ) return cand;
        }
    }

    const QString appCand = QDir::cleanPath(
        QDir( QCoreApplication::applicationDirPath() ).filePath( path ) );
    if( QFileInfo::exists( appCand ) ) return appCand;

    return path;
}

audio::twPluginSlotProcessor::Factory SPluginSlot::makeFactory() const
{
    // A FACTORY, not one instance: the dual-mono mapping (a 1->1 plugin on N
    // buses) needs one instance per bus, and instantiate() returns a fresh one
    // per call. The descriptor is captured BY VALUE so the factory stays valid
    // across a reloadPlugin() that replaces effective_.
    audio::twPluginDescriptor eff = effective_;
    // The initializer holder is shared, not copied: setInstanceInitializer()
    // after this factory was handed out must still reach its next call
    // (proposal 56 §4.3, invariant 4: a fresh instance is born current).
    std::shared_ptr<InstanceInitializer> init = initHolder_;
    return [eff, init]() {
        std::unique_ptr<audio::twPlugin> p = audio::pluginRegistry().instantiate( eff );
        if( p && init && *init ) ( *init )( *p );
        return p;
    };
}

void SPluginSlot::setInstanceInitializer( InstanceInitializer init )
{
    *initHolder_ = std::move( init );
}

std::size_t SPluginSlot::applyToInstances( const std::function<void( audio::twPlugin & )> &fn )
{
    std::size_t n = 0;
    if( proc_ ) n = proc_->applyToInstances( fn );
    // The processor staled its own pages under its lock; the path ABOVE the
    // slot is ours (see audioInvalidated() in the header).
    invalidateAbove();
    emit paramsChanged();
    return n;
}

// Proposal 36 B4: ONE insert, N channels wide, where there used to be one tap
// per bus. Two consequences worth naming, because both were awkward before:
//
//   * the width can go DOWN. ensureBuses() only ever grew, because shrinking
//     meant destroying components that a twPluginChain still held; now nothing
//     is created or destroyed and 8 -> 2 is one setChannelCount() call.
//   * re-deriving the mapping RE-INSTANTIATES the plugin, so the bypass flag
//     and the saved state chunk have to be re-applied every time the width
//     moves, not only the first time.
void SPluginSlot::ensureChannels( int nChannels ) const
{
    if( nChannels <= 0 ) return;

    SPluginSlot *self = const_cast<SPluginSlot *>( this );

    if( !self->proc_ ) {
        self->proc_ = std::make_shared<audio::twPluginSlotProcessor>(
            *SAppContext::get().get303aEnvironment(), makeFactory(), effective_.io );
        // Proposal 55 M2: a mask handed down before the processor existed
        // (a loaded project routes before every slot knows its width).
        if( self->routedMask_ ) self->proc_->setRoutedOutBuses( self->routedMask_ );
    }

    if( nChannels != self->channels_ ) {
        // setChannelCount() re-derives the mapping and prepare()s -- this runs
        // on the UI thread (insert-plugin action, project load,
        // STrack::setChannels), which is where CLAP says activate() belongs.
        self->proc_->setChannelCount( (idx_t) nChannels );
        self->channels_ = nChannels;
        self->proc_->setBypass( self->bypass_ );
        if( !self->savedState_.empty() ) {
            for( audio::twPlugin *p : self->proc_->plugins() )
                if( p ) p->loadState( self->savedState_ );
        }
    }

    if( !self->insert_ ) {
        self->insert_ = std::make_shared<audio::twPluginInsert>(
            *SAppContext::get().get303aEnvironment(), self->proc_ );
        // init() allocates the plugs and the output latch AND registers the
        // insert with the processor (which holds it weakly).
        self->insert_->init();
    }
}

void SPluginSlot::setChannelCount( int nChannels )
{
    ensureChannels( nChannels );
    // The lanes may have been READ before the processor existed - a load calls
    // readPostChildrenAttributes() long before STrack declares the width - so
    // this is where a project's stored automation actually reaches the DSP.
    pushParamCurves();
}

std::shared_ptr<audio::twPluginInsert> SPluginSlot::getInsert() const
{
    // A slot that has never been told its width still owes its caller a
    // component: default to mono, which the next setChannelCount() corrects
    // without rebuilding anything.
    ensureChannels( channels_ > 0 ? channels_ : 1 );
    return insert_;
}

std::shared_ptr<audio::twPluginInsert> SPluginSlot::peekInsert() const
{
    return insert_;
}

// --- aux routing (proposal 55 M2) -------------------------------------------

quint64 SPluginSlot::ensureSlotId()
{
    // Random, 63 bits, never 0 -- see the header for why not max+1 (T6).
    while( slotId_ == 0 )
        slotId_ = QRandomGenerator::global()->generate64() & 0x7fffffffffffffffULL;
    return slotId_;
}

bool SPluginSlot::setRoutedOutBuses( std::uint32_t mask )
{
    mask &= ~1u;   // bus 0 is the track's own output, never routed
    if( mask == routedMask_ ) return false;
    routedMask_ = mask;
    // NEVER materializes a processor: a slot with none yet gets the mask in
    // ensureChannels(). The processor stales the insert itself (param epoch).
    if( proc_ ) proc_->setRoutedOutBuses( mask );
    return true;
}

int SPluginSlot::auxOutBusCount() const
{
    if( proc_ && getSlotState() == audio::twPluginSlotState::Active ) {
        if( audio::twPlugin *p = proc_->plugin() )
            return (int) p->audioOutBusCount();
    }
    return (int) effective_.nOutBuses;
}

int SPluginSlot::auxOutBusChannels( int bus ) const
{
    if( bus < 0 ) return 0;
    if( proc_ && getSlotState() == audio::twPluginSlotState::Active ) {
        if( audio::twPlugin *p = proc_->plugin() ) {
            if( (std::size_t) bus >= p->audioOutBusCount() ) return 0;
            return (int) p->audioOutBus( (std::size_t) bus ).channels;
        }
    }
    if( bus < (int) effective_.outBusChannels.size() )
        return (int) effective_.outBusChannels[(std::size_t) bus];
    return 0;
}

// --- the sidechain (proposal 55 M4) -----------------------------------------

void SPluginSlot::setSidechain( SObject *source, bool preFader )
{
    scSource_ = source;
    scSet_    = true;
    scPre_    = preFader;
    scState_.clear();   // a new key is announced afresh
}

void SPluginSlot::clearSidechain()
{
    scSource_ = nullptr;
    scSet_    = false;
    scPre_    = true;
    scState_.clear();
}

int SPluginSlot::sidechainBusChannels() const
{
    if( proc_ && getSlotState() == audio::twPluginSlotState::Active ) {
        if( audio::twPlugin *p = proc_->plugin() ) {
            if( p->audioInBusCount() < 2 ) return 0;
            return (int) p->audioInBus( 1 ).channels;
        }
    }
    if( effective_.nInBuses >= 2 && effective_.inBusChannels.size() >= 2 )
        return (int) effective_.inBusChannels[1];
    return 0;
}

bool SPluginSlot::keyWiredTo( const twComponent *component ) const
{
    if( !insert_ || !component ) return false;
    return twProducerOf( insert_->getInputPlug( audio::twPluginInsert::kKeyPort ) ).get()
           == component;
}

bool SPluginSlot::keyWired() const
{
    return insert_ && insert_->keyWired();
}

bool SPluginSlot::applyKey( const std::shared_ptr<twComponent> &component )
{
    // NEVER materializes an insert for an unkeyed slot that has none.
    if( !component && !insert_ ) { keyHold_.reset(); return false; }
    std::shared_ptr<audio::twPluginInsert> ins = getInsert();
    if( !ins ) return false;

    // IDEMPOTENT, so a pass that changes nothing re-renders nothing: the plug
    // already reads this producer AND the processor already decided its D4
    // row for this producer's width. setKeySource() stales the insert's pages
    // (setInput bumps the epoch) and resets the plugin, so calling it on every
    // solo toggle would re-render every keyed slot from its first frame.
    if( component ) {
        const idx_t want = std::max<idx_t>( 1, component->getOutputChannels() );
        if( keyWiredTo( component.get() ) && proc_ && proc_->keyChannels() == want ) {
            keyHold_ = component;
            return false;
        }
    } else if( !ins->keyWired() ) {
        keyHold_.reset();
        return false;
    }
    ins->setKeySource( component );
    // Released AFTER the plug moved: the old producer must outlive the plug
    // that reads it.
    keyHold_ = component;
    return true;
}

QString SPluginSlot::describeKey() const
{
    return proc_ ? QString::fromStdString( proc_->describeKey() ) : QString();
}

// --- the instrument slot (proposal 37 P3b) ---------------------------------

void SPluginSlot::setEventSource( std::shared_ptr<const twEventSource> source )
{
    if( !source ) {
        // TAKING the feed away must never INSTANTIATE a plugin: this is also the
        // teardown path (STrack::onPluginSlotRemoved), which is the same reason
        // that path uses peekInsert() rather than getInsert().
        if( proc_ ) proc_->setEventSource( nullptr );
        return;
    }
    // ensureChannels(), not proc_ directly: a slot may still be waiting for its
    // width when the track wires the feed (project load orders the chain before
    // setChannels), and the source must not be dropped on the floor.
    ensureChannels( channels_ > 0 ? channels_ : 1 );
    if( proc_ ) proc_->setEventSource( std::move( source ) );
}

void SPluginSlot::setTempoMap( const twTempoMap &map )
{
    if( proc_ ) proc_->setTempoMap( map );
}

std::uint32_t SPluginSlot::tailFrames() const
{
    // Deliberately does NOT ensureChannels(): see the header. An un-materialized
    // slot reports no tail, and the first setChannelCount() makes it honest.
    return proc_ ? proc_->tailFrames() : 0u;
}

void SPluginSlot::forgetContinuity()
{
    if( proc_ ) proc_->forgetContinuity();
}

audio::twPluginSlotState SPluginSlot::getSlotState() const
{
    return proc_ ? proc_->state() : audio::twPluginSlotState::Missing;
}

audio::twPluginSlotMode SPluginSlot::getSlotMode() const
{
    return proc_ ? proc_->mode() : audio::twPluginSlotMode::Transparent;
}

bool SPluginSlot::reloadPlugin()
{
    // Re-ask the registry: a rescan since the project was loaded may have found
    // the module. Then hand the processor a new factory — deliberately NOT a new
    // processor, because the taps and every twPluginChain holding them reference
    // this one, and swapping it would mean re-wiring the whole DSP chain.
    resolveEffective();
    if( !proc_ ) {
        // Nothing was ever materialized (no bus count yet): the next
        // ensureBuses() will build from the freshly resolved descriptor.
        emit pluginReloaded();
        return false;
    }

    proc_->setFactory( makeFactory() );
    proc_->setBypass( bypass_ );
    // setFactory() rebuilds the instances; the curves are the processor's own
    // state and survive it, but re-pushing costs nothing and makes the two
    // re-resolution paths (this and setChannelCount) say the same thing.
    pushParamCurves();
    if( !savedState_.empty() ) {
        for( audio::twPlugin *p : proc_->plugins() )
            if( p ) p->loadState( savedState_ );
    }
    // setFactory() already staled the processor cache and every tap's pages; the
    // path DOWNSTREAM of the slot is the app's to invalidate.
    invalidateAbove();
    emit pluginReloaded();
    return getSlotState() == audio::twPluginSlotState::Active;
}

std::shared_ptr<twComponent> SPluginSlot::getRootComponent()
{
    auto insert = getInsert();
    if( insert ) {
        return std::static_pointer_cast<twComponent>(insert);
    }
    // Fallback (shouldn't happen - the insert exists even for a missing plugin;
    // the processor simply runs the transparent placeholder).
    throw std::runtime_error( "SPluginSlot: no plugin insert available" );
}

QWidget *SPluginSlot::getDetailEditWidget( QWidget *parent )
{
    // Deliberately null. The generic parameter editor is SPluginParamEditor in
    // app/pluginui, and objects/track may not reach the UI layer (this module is
    // in app_objects; pluginui is app_ui). The FX strip owns the editor and
    // opens it on a double-click — see splugineffectstrip.cpp — which also keeps
    // the trackPath/slotIndex the parameter action needs in the one place that
    // actually knows them.
    (void) parent;
    return nullptr;
}

QWidget *SPluginSlot::getInlineEditWidget( QWidget *parent )
{
    return nullptr;
}

SObjectRenderer *SPluginSlot::getInlineRenderer()
{
    return nullptr;
}

// --- serialization -----------------------------------------------------------

int SPluginSlot::readPreChildrenAttributes( QDomElement &element )
{
    SObject::readPreChildrenAttributes( element );

    if( element.hasAttribute( "bypassed" ) )
        bypass_ = element.attribute( "bypassed" ) == "true";

    if( element.hasAttribute( "editorOpen" ) )
        editorOpen_ = element.attribute( "editorOpen" ) == "true";

    // Proposal 55 D8: absent = never referenced by a return.
    if( element.hasAttribute( "slotId" ) )
        slotId_ = element.attribute( "slotId" ).toULongLong();

    // Read the opaque state chunk (base64 encoded in child <state> element)
    QDomElement stateElem = element.firstChildElement( "state" );
    if( !stateElem.isNull() ) {
        QString encoded = stateElem.text();
        QByteArray decoded = QByteArray::fromBase64( encoded.toLatin1() );
        savedState_.resize( decoded.size() );
        std::copy( decoded.begin(), decoded.end(), savedState_.begin() );

        // Restore state to every instance the slot already has (dual-mono has N).
        // On the load path there is none yet — the blob is kept and replayed by
        // ensureBuses() once STrack declares the bus count.
        restoreState( savedState_ );
    }

    return 0;
}

// The wire schema of a slot (proposal 08 §Layer 6). SObject::serializeSelfAttributes
// FIRST and unconditionally: it is what emits id=, and without it
// SProjectLoader::createObjects hits `if( id.isNull() ) … return -1` and the
// WHOLE project load aborts — not just this slot.
//
//   <SPluginSlot id='…' … bypassed='false' format='clap' uid='…' name='…'
//                vendor='…' path='…' nIn='2' nOut='2' isInstrument='false'>
//     <state encoding='base64'>…</state>
//   </SPluginSlot>
//
// descriptor_ is what is written, never effective_: the stored descriptor is
// exactly what the project file said, so a project stays portable (a relative
// module path is not silently absolutized) and a MISSING plugin's identity
// survives a save on a machine that does not have it.
int SPluginSlot::serializeSelfAttributes( QTextStream &o )
{
    SObject::serializeSelfAttributes( o );
    o << " bypassed='" << (bypass_ ? "true" : "false") << "'";
    o << " format='" << attrEscape( descriptor_.format ) << "'";
    o << " uid='" << attrEscape( descriptor_.uid ) << "'";
    o << " name='" << attrEscape( descriptor_.name ) << "'";
    o << " vendor='" << attrEscape( descriptor_.vendor ) << "'";
    o << " path='" << attrEscape( descriptor_.path ) << "'";
    o << " nIn='" << descriptor_.io.audioInputs << "'";
    o << " nOut='" << descriptor_.io.audioOutputs << "'";
    o << " isInstrument='" << (descriptor_.isInstrument ? "true" : "false") << "'";
    // Proposal 55 D8: the stable id a return track's <auxSource slot=> names.
    // Written only once something has referenced the slot, so a project that
    // never routed an aux bus re-serializes byte-identically (T8).
    if( slotId_ != 0 )
        o << " slotId='" << slotId_ << "'";
    // WRITTEN ONLY WHEN TRUE, which is what keeps every project file saved
    // before proposal 33 D2 — and both render goldens — byte-unchanged. The
    // same discipline SObject::serialize() already follows for automation
    // lanes.
    if( editorOpen_ )
        o << " editorOpen='true'";
    return 0;
}

// SObject::serialize() only writes attributes and SLink children, so the state
// chunk needs this override. It replaces the DOM-based serializeStateChunk()
// that M4 deleted: that function had ZERO callers, because the write path is
// QTextStream-based — which is what made the base64 restore in
// readPreChildrenAttributes dead code and slots un-round-trippable.
//
// The blob is pulled FRESH from the live plugin at save time (saveState()), so a
// parameter the user moved is in the file even though nothing told the slot
// about it. Output is deterministic: base64 of the same bytes, no timestamps, no
// pointer values beyond the id= the base class already writes.
int SPluginSlot::serialize( QTextStream &o )
{
    const char *cls = metaObject()->className();

    o << "<" << cls;
    int res = serializeSelfAttributes( o );
    if( res < 0 ) return res;
    o << ">\n";

    // Inline `<automation>` next to `<state>` (design §3.3). SObject::serialize()
    // emits it for every other owner; this override has to do it by hand for
    // exactly the same reason it writes `<state>` by hand.
    serializeAutomation( o );

    std::vector<std::uint8_t> state;
    saveState( state );
    if( !state.empty() ) {
        const QByteArray raw( (const char *) state.data(), (int) state.size() );
        o << "<state encoding='base64'>" << QString::fromLatin1( raw.toBase64() )
          << "</state>\n";
    }

    // THE SIDECHAIN (proposal 55 D8), ONLY WHEN SET, so a project that never
    // keyed a slot re-serializes byte-identically (T8). The source is an
    // object id exactly as <auxSource track=> and pluginChainId are; a source
    // that did not resolve is written as source='0' so the slot stays keyed
    // (and its key silent) across a save.
    if( scSet_ ) {
        const std::uintptr_t src = scSource_
            ? reinterpret_cast<std::uintptr_t>( scSource_.data() ) : 0;
        o << "<sidechain source='" << src << "' tap='"
          << ( scPre_ ? "pre" : "post" ) << "'/>\n";
    }

    // The slot's MODEL CHILDREN (proposal 56 B4): an SLink per child, the
    // base class's shape. Since Q1 a NassauDigital slot has one, its program;
    // every other slot still has none, so their files are byte-unchanged.
    for( SLink *lk : childLinks() ) {
        int r = lk->serialize( o );
        if( r < 0 ) break;
    }

    o << "</" << cls << ">\n";
    return 0;
}

// Modelled on STakeStack::instantiateFromDomElement (objects/cut). Rebuilds the
// descriptor from the attributes and lets the constructor resolve it against the
// registry by (format, uid).
SLink *SPluginSlot::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    (void) parent;

    audio::twPluginDescriptor desc;
    desc.format = element.attribute( "format" ).toStdString();
    desc.uid    = element.attribute( "uid" ).toStdString();
    desc.name   = element.attribute( "name" ).toStdString();
    desc.vendor = element.attribute( "vendor" ).toStdString();
    desc.path   = element.attribute( "path" ).toStdString();
    desc.io.audioInputs  =
        (std::uint16_t) element.attribute( "nIn",  "0" ).toUInt();
    desc.io.audioOutputs =
        (std::uint16_t) element.attribute( "nOut", "0" ).toUInt();
    desc.isInstrument = element.attribute( "isInstrument", "false" ) == "true";

    SPluginSlot *slot = new SPluginSlot( &projectLoader.getProject(), desc );
    slot->readPreChildrenAttributes( element );

    // The model children (proposal 56 B4, Q1). The loader instantiates an
    // element only once every <SLink> child of it resolves, so each target is
    // in the dictionary here; one that is not was pruned (the slot is a
    // CONTAINER kind, below) and costs only its own link.
    QList<SObject *> adopted;
    for( QDomElement c = element.firstChildElement( "SLink" ); !c.isNull();
         c = c.nextSiblingElement( "SLink" ) ) {
        const QString objectId = c.attribute( "objectId" );
        SLink *target = projectLoader.getObjectDictionary().value( objectId );
        if( !target ) {
            qWarning( "SPluginSlot: child object %s is not in the file; skipped",
                      qPrintable( objectId ) );
            continue;
        }
        new SLink( target->getSObject(), slot );
        adopted.append( &target->getSObject() );
    }

    // THE SIDECHAIN (proposal 55 D8), DEFERRED for pluginChainId's reason:
    // the key lane's element may not have been instantiated yet. A source id
    // that does not resolve FAILS CLOSED: the slot stays keyed with no source,
    // the mixer pass announces the key silent, and it is never re-attached by
    // a later object (T6).
    const QDomElement scEl = element.firstChildElement( "sidechain" );
    if( !scEl.isNull() ) {
        slot->scSet_ = true;
        slot->scPre_ = scEl.attribute( "tap", "pre" ) != QLatin1String( "post" );
        const QString srcId = scEl.attribute( "source" );
        SProjectLoader *loader = &projectLoader;
        QPointer<SPluginSlot> guard( slot );
        projectLoader.deferResolve( [loader, guard, srcId]() {
            if( !guard ) return;
            SLink *lk = loader->getObjectDictionary().value( srcId );
            SObject *src = lk ? &lk->getSObject() : nullptr;
            if( !src || !src->isLane() ) {
                TW_LOGW( "model", "[SIDECHAIN] '%s': its key source %s is not in the "
                         "project; the key stays silent",
                         guard->getSName().toUtf8().constData(),
                         srcId.toUtf8().constData() );
                return;
            }
            guard->scSource_ = src;
        } );
    }

    slot->readPostChildrenAttributes( element );
    for( SObject *child : adopted ) slot->notifyChildAdopted_( *child );
    return new SLink( *slot );
}

namespace {
SPluginSlot::ChildAdoptedHook &childAdoptedHook()
{
    static SPluginSlot::ChildAdoptedHook hook;
    return hook;
}
}  // namespace

void SPluginSlot::setChildAdoptedHook( ChildAdoptedHook hook )
{
    childAdoptedHook() = std::move( hook );
}

SLink *SPluginSlot::adoptChild( SObject &child )
{
    SLink *lk = new SLink( child, this );
    notifyChildAdopted_( child );
    return lk;
}

void SPluginSlot::notifyChildAdopted_( SObject &child )
{
    if( childAdoptedHook() ) childAdoptedHook()( *this, child );
}

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_spluginslot =
    ( SProjectLoader::registerSObjectClass( "SPluginSlot",
          SPluginSlot::instantiateFromDomElement,
          // A CONTAINER since proposal 56 Q1, when a slot gained model
          // children: a child the file lost (or that failed to load) costs its
          // own link, never the slot -- the user's plugin and its state chunk
          // still load (proposal 37 D8a's rule for a container).
          SElementKind::Container ), true );

// --- runtime control ---------------------------------------------------------

// --- automation (proposal 37 P5) --------------------------------------------

void SPluginSlot::pushParamCurves()
{
    if( !proc_ ) return;
    std::map<std::uint32_t, std::shared_ptr<const twAutomationCurve> > curves;
    for( SAutomationLane *lane : automationLanes() ) {
        if( !lane || lane->ref().space != SParamRef::Space::Param ) continue;
        std::shared_ptr<const twAutomationCurve> snap = lane->snapshot();
        if( !snap ) continue;      // absent / empty / Off == the scalar path
        curves[lane->ref().paramId] = std::move( snap );
    }
    // setParamCurves() bumps automationEpoch_ AND stales our insert's pages;
    // what it CANNOT do is reach the twPluginChain / twTrackMix / mixer pages
    // above us. That half is onAutomationChanged()'s.
    proc_->setParamCurves( std::move( curves ) );
}

void SPluginSlot::applyAutomationToEngine()
{
    pushParamCurves();
}

void SPluginSlot::onAutomationChanged( SAutomationLane &lane,
                                       offset_t start, offset_t end )
{
    (void) lane;
    pushParamCurves();
    // NOT SObject::invalidateRenderPathRange(): that walks DOWN from the
    // project root through childLinks() looking for `this`, and an SPluginChain
    // is deliberately not an SLink child of its track, so the walk never
    // arrives (proposal 08 M5's finding, pluginui inv. 6). The owning STrack
    // does it for us.
    emit audioInvalidatedRange( (qint64) start, (qint64) end );
}

void SPluginSlot::setBypass( bool bypass )
{
    if( bypass_ != bypass ) {
        bypass_ = bypass;
        // One call reaches every bus: the flag lives on the shared processor,
        // which also stales the taps' frozen pages (without which the toggle
        // would be inaudible — the cached page would be served unchanged).
        if( proc_ ) proc_->setBypass( bypass );
        invalidateAbove();
        emit bypassChanged( bypass );
    }
}

void SPluginSlot::notifyPluginEdited()
{
    if( proc_ ) proc_->bumpParamEpoch();
    invalidateAbove();
    emit paramsChanged();
}

// Stale everything ABOVE this slot. See the audioInvalidated() comment in the
// header for why SObject::invalidateRenderPath() cannot do this on its own: the
// chain is not an SLink child of the track, so the root-down walk never finds a
// slot. It is still called, because it is correct and free, and because a future
// model change that DOES link the chain into the tree would make it the right
// answer; the signal is what actually reaches the track today.
void SPluginSlot::invalidateAbove()
{
    invalidateRenderPath();
    emit audioInvalidated();
}

void SPluginSlot::saveState( std::vector<std::uint8_t> &state )
{
    // Bus 0's instance is the representative: in every supported mapping except
    // dual-mono there is only one, and dual-mono instances are kept in lockstep
    // by restoreState()/notifyPluginEdited().
    //
    // ONLY when the slot is Active. A Missing slot runs createNullPlugin()'s
    // placeholder, whose state chunk is empty — reading it would overwrite the
    // absent plugin's settings with nothing, i.e. a user would lose their patch
    // by opening the project on a machine where the plugin is not installed and
    // saving it. Unsupported is treated the same way for the same reason.
    if( proc_ && getSlotState() == audio::twPluginSlotState::Active ) {
        if( audio::twPlugin *p = proc_->plugin() ) {
            state = p->saveState();
            savedState_ = state;
            return;
        }
    }
    state = savedState_;
}

void SPluginSlot::restoreState( const std::vector<std::uint8_t> &state )
{
    savedState_ = state;
    if( !proc_ ) return;
    for( audio::twPlugin *p : proc_->plugins() )
        if( p ) p->loadState( state );
    // A state chunk changes what process() produces, so the cached pages have to
    // go with it.
    proc_->bumpParamEpoch();
    emit paramsChanged();
}

// --- proposal 37 P6: the knobs, in APP types ---------------------------------
//
// The automation lane picker (app/timeline) needs a parameter's name and its
// declared range to label a lane and to scale it. It may not include
// tw/plugins (tools/check_layering.py), and it should not have to: the slot
// already owns the plugin, so the translation belongs here.
// PROPOSAL 21 L5. Reported, never compensated - see the header.
std::uint32_t SPluginSlot::reportedLatencyFrames() const
{
    if( !proc_ ) return 0;
    audio::twPlugin *plugin = proc_->plugin();
    return plugin ? plugin->reportedLatency() : 0u;
}

QVector<SPluginSlot::ParamRow> SPluginSlot::paramRows() const
{
    QVector<ParamRow> out;
    if( !proc_ ) return out;
    audio::twPlugin *plugin = proc_->plugin();
    if( !plugin ) return out;
    const std::size_t n = plugin->paramCount();
    out.reserve( (int) n );
    for( std::size_t i = 0; i < n; ++i ) {
        const audio::twPluginParamInfo info = plugin->paramInfo( i );
        ParamRow r;
        r.id           = info.id;
        r.name         = QString::fromStdString( info.name );
        r.minValue     = info.minValue;
        r.maxValue     = info.maxValue;
        r.defaultValue = info.defaultValue;
        r.isStepped    = info.isStepped;
        out.append( r );
    }
    return out;
}
