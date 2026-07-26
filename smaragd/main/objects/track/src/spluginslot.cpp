#include "app/objects/track/spluginslot.h"
#include "app/model/sproject.h"
#include "app/model/sappcontext.h"
#include "tw/plugins/twplugin.h"
#include "tw/plugins/twplugininsert.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/plugins/twplugindescriptor.h"
#include <QDomElement>
#include <QTextStream>
#include <QByteArray>

SPluginSlot::SPluginSlot( SProject *project, const audio::twPluginDescriptor &desc )
    : SObject( project ), descriptor_( desc ), effective_( desc )
{
    setSName( QString::fromStdString( desc.name ) );

    // Resolve (format, uid) against the registry: a scanned record carries the
    // module PATH and the plugin's REAL channel counts (the scanner instantiated
    // it to read them), both of which a hand-built or older-project descriptor
    // may be missing or wrong about. Falls back to what we were given, which is
    // what keeps a headless run without a scan working.
    audio::twPluginDescriptor resolved;
    if( audio::pluginRegistry().findByUid( descriptor_.format, descriptor_.uid,
                                          resolved ) ) {
        effective_ = resolved;
    }

    // The processor and its taps are created on demand (setBusCount / the first
    // getInsertForBus), because the bus count is what decides the
    // channel-mismatch mapping and only STrack knows it.
}

SPluginSlot::~SPluginSlot() = default;

void SPluginSlot::ensureBuses( int nBuses ) const
{
    if( nBuses <= 0 ) return;

    SPluginSlot *self = const_cast<SPluginSlot *>( this );

    if( !self->proc_ ) {
        // A FACTORY, not one instance: the dual-mono mapping (a 1->1 plugin on
        // N buses) needs one instance per bus, and instantiate() returns a fresh
        // one per call.
        audio::twPluginDescriptor eff = effective_;
        self->proc_ = std::make_shared<audio::twPluginSlotProcessor>(
            *SAppContext::get().get303aEnvironment(),
            [eff]() { return audio::pluginRegistry().instantiate( eff ); },
            eff.io );
    }

    if( nBuses > self->busCount_ ) {
        // setBusCount() re-derives the mapping and prepare()s — this runs on the
        // UI thread (insert-plugin action, project load, STrack::setNBusses),
        // which is where CLAP says activate() belongs.
        self->proc_->setBusCount( (idx_t) nBuses );
        self->busCount_ = nBuses;
        self->proc_->setBypass( self->bypass_ );
        if( !self->savedState_.empty() ) {
            for( audio::twPlugin *p : self->proc_->plugins() )
                if( p ) p->loadState( self->savedState_ );
        }
    }

    while( (int) self->taps_.size() < nBuses ) {
        const idx_t bus = (idx_t) self->taps_.size();
        auto tap = std::make_shared<audio::twPluginInsert>(
            *SAppContext::get().get303aEnvironment(), self->proc_, bus );
        // init() allocates the plugs and the output latch AND registers the tap
        // with the processor (which holds it weakly).
        tap->init();
        self->taps_.push_back( std::move( tap ) );
    }
}

void SPluginSlot::setBusCount( int nBuses )
{
    ensureBuses( nBuses );
}

std::shared_ptr<audio::twPluginInsert> SPluginSlot::getInsertForBus( int busIndex ) const
{
    if( busIndex < 0 ) return nullptr;
    ensureBuses( busIndex + 1 );
    if( busIndex >= (int) taps_.size() ) return nullptr;
    return taps_[busIndex];
}

audio::twPluginSlotState SPluginSlot::getSlotState() const
{
    return proc_ ? proc_->state() : audio::twPluginSlotState::Missing;
}

audio::twPluginSlotMode SPluginSlot::getSlotMode() const
{
    return proc_ ? proc_->mode() : audio::twPluginSlotMode::Transparent;
}

std::shared_ptr<twComponent> SPluginSlot::getRootComponent()
{
    auto tap = getInsertForBus(0);
    if( tap ) {
        return std::static_pointer_cast<twComponent>(tap);
    }
    // Fallback (shouldn't happen - the tap exists even for a missing plugin;
    // the processor simply loads transparent).
    throw std::runtime_error( "SPluginSlot: no plugin insert available" );
}

QWidget *SPluginSlot::getDetailEditWidget( QWidget *parent )
{
    // TODO: parameter editor widget (proposal 08 M5)
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

int SPluginSlot::readPreChildrenAttributes( QDomElement &element )
{
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
        restoreState( savedState_ );
    }

    return 0;
}

int SPluginSlot::serializeSelfAttributes( QTextStream &o )
{
    o << " bypassed='" << (bypass_ ? "true" : "false") << "'";
    o << " format='" << QString::fromStdString( descriptor_.format ) << "'";
    o << " uid='" << QString::fromStdString( descriptor_.uid ) << "'";
    o << " vendor='" << QString::fromStdString( descriptor_.vendor ) << "'";
    o << " nIn='" << descriptor_.io.audioInputs << "'";
    o << " nOut='" << descriptor_.io.audioOutputs << "'";
    return 0;
}

void SPluginSlot::setBypass( bool bypass )
{
    if( bypass_ != bypass ) {
        bypass_ = bypass;
        // One call reaches every bus: the flag lives on the shared processor,
        // which also stales the taps' frozen pages (without which the toggle
        // would be inaudible — the cached page would be served unchanged).
        if( proc_ ) proc_->setBypass( bypass );
        invalidateRenderPath();
        emit bypassChanged( bypass );
    }
}

void SPluginSlot::notifyPluginEdited()
{
    if( proc_ ) proc_->bumpParamEpoch();
    invalidateRenderPath();
}

void SPluginSlot::saveState( std::vector<std::uint8_t> &state )
{
    // Bus 0's instance is the representative: in every supported mapping except
    // dual-mono there is only one, and dual-mono instances are kept in lockstep
    // by restoreState()/notifyPluginEdited().
    if( proc_ ) {
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
}

void SPluginSlot::serializeStateChunk( QDomElement &parentElem, QDomDocument &doc )
{
    // Save current state from the plugin
    std::vector<std::uint8_t> state;
    saveState( state );

    if( !savedState_.empty() ) {
        QDomElement stateElem = doc.createElement( "state" );
        stateElem.setAttribute( "encoding", "base64" );

        QByteArray data( (const char*)savedState_.data(), (int)savedState_.size() );
        QString encoded = QString::fromLatin1( data.toBase64() );
        stateElem.appendChild( doc.createTextNode( encoded ) );

        parentElem.appendChild( stateElem );
    }
}
