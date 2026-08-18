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
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QDomElement>
#include <QFileInfo>
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

SPluginSlot::~SPluginSlot() = default;

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
    return [eff]() { return audio::pluginRegistry().instantiate( eff ); };
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

    // A slot has no SLink children today; keep the base class's shape so it
    // stays correct if one is ever added (a side-chain source, say).
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
    slot->readPostChildrenAttributes( element );
    return new SLink( *slot );
}

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_spluginslot =
    ( SProjectLoader::registerSObjectClass( "SPluginSlot",
          SPluginSlot::instantiateFromDomElement ), true );

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
