// VST3 host-side objects (proposal 08 M6). See twvst3host.h for the ownership
// model — the one thing worth reading before this file.

#include "twvst3host.h"

#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"

#include "tw/core/twlog.h"

#include <algorithm>
#include <cstring>

using namespace Steinberg;

namespace audio {

namespace {

// UTF-16 code units for a plain ASCII literal, bounded by the SDK's String128.
void copyToString128( Vst::String128 dst, const char *src )
{
    std::size_t i = 0;
    for( ; src[i] && i < 127; ++i ) dst[i] = (Vst::TChar)(unsigned char)src[i];
    dst[i] = 0;
}

std::size_t u16Len( const Vst::TChar *s )
{
    std::size_t n = 0;
    while( s && s[n] ) ++n;
    return n;
}

}  // namespace

// --- twVst3MemStream ----------------------------------------------------------

tresult PLUGIN_API twVst3MemStream::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, IBStream::iid );
}

tresult PLUGIN_API twVst3MemStream::read( void *buffer, int32 numBytes, int32 *numBytesRead )
{
    if( !buffer || numBytes < 0 ) return kInvalidArgument;
    const int64 avail = (int64)data_.size() - pos_;
    const int32 n = (int32)std::min<int64>( numBytes, std::max<int64>( 0, avail ) );
    if( n > 0 ) std::memcpy( buffer, data_.data() + pos_, (std::size_t)n );
    pos_ += n;
    if( numBytesRead ) *numBytesRead = n;
    return kResultOk;
}

tresult PLUGIN_API twVst3MemStream::write( void *buffer, int32 numBytes, int32 *numBytesWritten )
{
    if( !buffer || numBytes < 0 ) return kInvalidArgument;
    if( pos_ + numBytes > (int64)data_.size() )
        data_.resize( (std::size_t)( pos_ + numBytes ) );
    std::memcpy( data_.data() + pos_, buffer, (std::size_t)numBytes );
    pos_ += numBytes;
    if( numBytesWritten ) *numBytesWritten = numBytes;
    return kResultOk;
}

tresult PLUGIN_API twVst3MemStream::seek( int64 pos, int32 mode, int64 *result )
{
    int64 base = 0;
    if( mode == kIBSeekSet )      base = 0;
    else if( mode == kIBSeekCur ) base = pos_;
    else if( mode == kIBSeekEnd ) base = (int64)data_.size();
    else return kInvalidArgument;

    const int64 want = base + pos;
    if( want < 0 ) return kInvalidArgument;
    pos_ = want;
    if( result ) *result = pos_;
    return kResultOk;
}

tresult PLUGIN_API twVst3MemStream::tell( int64 *pos )
{
    if( !pos ) return kInvalidArgument;
    *pos = pos_;
    return kResultOk;
}

// --- twVst3AttributeList ------------------------------------------------------

tresult PLUGIN_API twVst3AttributeList::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, Vst::IAttributeList::iid );
}

tresult PLUGIN_API twVst3AttributeList::setInt( AttrID id, int64 value )
{
    if( !id ) return kInvalidArgument;
    Value v;
    v.kind = Kind::Int;
    v.i    = value;
    values_[id] = std::move( v );
    return kResultOk;
}

tresult PLUGIN_API twVst3AttributeList::getInt( AttrID id, int64 &value )
{
    if( !id ) return kInvalidArgument;
    const auto it = values_.find( id );
    if( it == values_.end() || it->second.kind != Kind::Int ) return kResultFalse;
    value = it->second.i;
    return kResultOk;
}

tresult PLUGIN_API twVst3AttributeList::setFloat( AttrID id, double value )
{
    if( !id ) return kInvalidArgument;
    Value v;
    v.kind = Kind::Float;
    v.f    = value;
    values_[id] = std::move( v );
    return kResultOk;
}

tresult PLUGIN_API twVst3AttributeList::getFloat( AttrID id, double &value )
{
    if( !id ) return kInvalidArgument;
    const auto it = values_.find( id );
    if( it == values_.end() || it->second.kind != Kind::Float ) return kResultFalse;
    value = it->second.f;
    return kResultOk;
}

tresult PLUGIN_API twVst3AttributeList::setString( AttrID id, const Vst::TChar *string )
{
    if( !id || !string ) return kInvalidArgument;
    const std::size_t n = u16Len( string );
    Value v;
    v.kind = Kind::String;
    v.bytes.resize( ( n + 1 ) * sizeof( Vst::TChar ) );   // keep the terminator
    std::memcpy( v.bytes.data(), string, v.bytes.size() );
    values_[id] = std::move( v );
    return kResultOk;
}

tresult PLUGIN_API twVst3AttributeList::getString( AttrID id, Vst::TChar *string,
                                                   uint32 sizeInBytes )
{
    if( !id || !string || sizeInBytes < sizeof( Vst::TChar ) ) return kInvalidArgument;
    const auto it = values_.find( id );
    if( it == values_.end() || it->second.kind != Kind::String ) return kResultFalse;

    // sizeInBytes is a BYTE count, not a character count — a distinction that
    // has cost other hosts a buffer overrun. Always terminate.
    const std::size_t maxChars = sizeInBytes / sizeof( Vst::TChar );
    const std::size_t haveChars = it->second.bytes.size() / sizeof( Vst::TChar );
    const std::size_t n = std::min( maxChars ? maxChars - 1 : 0,
                                    haveChars ? haveChars - 1 : 0 );
    if( n ) std::memcpy( string, it->second.bytes.data(), n * sizeof( Vst::TChar ) );
    string[n] = 0;
    return kResultOk;
}

tresult PLUGIN_API twVst3AttributeList::setBinary( AttrID id, const void *data,
                                                   uint32 sizeInBytes )
{
    if( !id || ( !data && sizeInBytes ) ) return kInvalidArgument;
    Value v;
    v.kind = Kind::Binary;
    v.bytes.assign( (const std::uint8_t *)data,
                    (const std::uint8_t *)data + sizeInBytes );
    values_[id] = std::move( v );
    return kResultOk;
}

tresult PLUGIN_API twVst3AttributeList::getBinary( AttrID id, const void *&data,
                                                   uint32 &sizeInBytes )
{
    if( !id ) return kInvalidArgument;
    const auto it = values_.find( id );
    if( it == values_.end() || it->second.kind != Kind::Binary ) return kResultFalse;
    data        = it->second.bytes.data();
    sizeInBytes = (uint32)it->second.bytes.size();
    return kResultOk;
}

// --- twVst3Message ------------------------------------------------------------

twVst3Message::twVst3Message() : attrs_( new twVst3AttributeList() ) {}

twVst3Message::~twVst3Message()
{
    if( attrs_ ) attrs_->release();
}

tresult PLUGIN_API twVst3Message::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, Vst::IMessage::iid );
}

FIDString PLUGIN_API twVst3Message::getMessageID()
{
    return id_.empty() ? nullptr : id_.c_str();
}

void PLUGIN_API twVst3Message::setMessageID( FIDString id )
{
    id_ = id ? id : "";
}

Vst::IAttributeList *PLUGIN_API twVst3Message::getAttributes()
{
    // Borrowed by the caller for the duration of the message: the SDK's contract
    // is that the attribute list belongs to the message, so no addRef here.
    return attrs_;
}

// --- twVst3PlugInterfaceSupport -----------------------------------------------

tresult PLUGIN_API twVst3PlugInterfaceSupport::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, Vst::IPlugInterfaceSupport::iid );
}

tresult PLUGIN_API twVst3PlugInterfaceSupport::isPlugInterfaceSupported( const TUID _iid )
{
    // Only what the backend genuinely implements. Claiming support for an
    // interface we do not answer is how a host earns a crash inside the plugin,
    // so the default is no.
    if( FUnknownPrivate::iidEqual( _iid, Vst::IComponentHandler::iid ) ||
        FUnknownPrivate::iidEqual( _iid, Vst::IHostApplication::iid ) ||
        FUnknownPrivate::iidEqual( _iid, Vst::IAttributeList::iid ) ||
        FUnknownPrivate::iidEqual( _iid, Vst::IMessage::iid ) ||
        FUnknownPrivate::iidEqual( _iid, Vst::IParameterChanges::iid ) ||
        FUnknownPrivate::iidEqual( _iid, Vst::IParamValueQueue::iid ) ||
        // proposal 37 P2. IEventList is implemented (twVst3EventList, both
        // directions); IMidiMapping and INoteExpressionController are QUERIED on
        // the controller rather than implemented, and this list is about what a
        // plugin may expect the HOST to understand — a plugin that sees them
        // knows its CC map and its note expressions will be asked for and used.
        FUnknownPrivate::iidEqual( _iid, Vst::IEventList::iid ) ||
        FUnknownPrivate::iidEqual( _iid, Vst::IMidiMapping::iid ) ||
        FUnknownPrivate::iidEqual( _iid, Vst::INoteExpressionController::iid ) ||
        FUnknownPrivate::iidEqual( _iid, IBStream::iid ) )
        return kResultTrue;
    return kResultFalse;
}

// --- twVst3HostApplication ----------------------------------------------------

twVst3HostApplication::twVst3HostApplication( std::string instanceName )
    : name_( std::move( instanceName ) )
{
}

tresult PLUGIN_API twVst3HostApplication::queryInterface( const TUID _iid, void **obj )
{
    if( FUnknownPrivate::iidEqual( _iid, Vst::IPlugInterfaceSupport::iid ) ) {
        *obj = &support_;
        support_.addRef();
        return kResultOk;
    }
    return resolveOne( _iid, obj, Vst::IHostApplication::iid );
}

tresult PLUGIN_API twVst3HostApplication::getName( Vst::String128 name )
{
    if( !name ) return kInvalidArgument;
    copyToString128( name, name_.empty() ? "Smaragd" : name_.c_str() );
    return kResultOk;
}

tresult PLUGIN_API twVst3HostApplication::createInstance( TUID cid, TUID _iid, void **obj )
{
    if( !obj ) return kInvalidArgument;
    *obj = nullptr;

    // The only two the spec asks a host to manufacture. Both are how a split
    // component/controller pair talks to itself through us.
    if( FUnknownPrivate::iidEqual( cid, Vst::IMessage::iid ) &&
        FUnknownPrivate::iidEqual( _iid, Vst::IMessage::iid ) ) {
        *obj = new twVst3Message();
        return kResultOk;
    }
    if( FUnknownPrivate::iidEqual( cid, Vst::IAttributeList::iid ) &&
        FUnknownPrivate::iidEqual( _iid, Vst::IAttributeList::iid ) ) {
        *obj = new twVst3AttributeList();
        return kResultOk;
    }
    return kNotImplemented;
}

// --- twVst3ComponentHandler ---------------------------------------------------

tresult PLUGIN_API twVst3ComponentHandler::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, Vst::IComponentHandler::iid );
}

// The three gesture callbacks all funnel here (proposal 33 M2). RECORDING, not
// acting: this can arrive on the plugin's own UI thread, and this repo has
// already paid for reaching into graph state from a non-main thread. The
// backend drains where it is safe to act.
void twVst3ComponentHandler::record_( Vst::ParamID id, double value,
                                      twEditorGesture phase )
{
    std::lock_guard<std::mutex> lock( editMutex_ );

    if( edits_.size() >= kMaxPendingEdits ) {
        // Nobody is draining, or the plugin is streaming values faster than the
        // 30 Hz poll. Drop the oldest CHANGE and keep every Begin and End: an
        // intermediate position of a knob is worth nothing once a newer one
        // exists, whereas losing an End leaves an undo entry and an automation
        // punch-in open forever. Scanning from the front is O(n) on a path that
        // only runs when something is already pathological.
        auto victim = std::find_if( edits_.begin(), edits_.end(),
                                    []( const Edit &e ) {
                                        return e.phase == twEditorGesture::Change;
                                    } );
        if( victim != edits_.end() ) edits_.erase( victim );
        else                         edits_.erase( edits_.begin() );
    }

    edits_.push_back( Edit{ (std::uint32_t)id, value, phase } );
    edited_.store( true, std::memory_order_release );
}

std::vector<twVst3ComponentHandler::Edit> twVst3ComponentHandler::takeEdits()
{
    std::lock_guard<std::mutex> lock( editMutex_ );
    std::vector<Edit> out;
    out.swap( edits_ );
    return out;
}

tresult PLUGIN_API twVst3ComponentHandler::beginEdit( Vst::ParamID id )
{
    // The value is deliberately not read here. VST3 gives beginEdit no value,
    // and querying the controller for one would be a call INTO the plugin from
    // inside its own callback — re-entrancy some plugins do not survive. The
    // first performEdit carries it.
    record_( id, 0.0, twEditorGesture::Begin );
    return kResultOk;
}

tresult PLUGIN_API twVst3ComponentHandler::performEdit( Vst::ParamID id,
                                                        Vst::ParamValue v )
{
    // Both arguments used to be DISCARDED here, which is why a native editor
    // could never have been audible: the host learned that something changed
    // and never what. `v` is VST3-normalized [0,1], which is already the
    // host-facing domain twPlugin::setParam() and `param:` automation lanes
    // speak, so no conversion belongs on this path.
    record_( id, (double)v, twEditorGesture::Change );
    return kResultOk;
}

tresult PLUGIN_API twVst3ComponentHandler::endEdit( Vst::ParamID id )
{
    record_( id, 0.0, twEditorGesture::End );
    return kResultOk;
}

tresult PLUGIN_API twVst3ComponentHandler::restartComponent( int32 flags )
{
    // kLatencyChanged / kParamValuesChanged / kReloadComponent all land here and
    // used to be indistinguishable — the body was `(void)flags;`. They are OR-ed
    // rather than replaced, so two restarts between drains do not lose the first
    // one's reason. Recorded, never acted on from this thread.
    restartFlags_.fetch_or( flags, std::memory_order_acq_rel );
    restart_.store( true, std::memory_order_release );
    return kResultOk;
}

// --- twVst3ParamValueQueue ----------------------------------------------------

tresult PLUGIN_API twVst3ParamValueQueue::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, Vst::IParamValueQueue::iid );
}

tresult PLUGIN_API twVst3ParamValueQueue::getPoint( int32 index, int32 &sampleOffset,
                                                    Vst::ParamValue &value )
{
    if( index < 0 || (std::size_t)index >= points_.size() ) return kInvalidArgument;
    sampleOffset = points_[(std::size_t)index].offset;
    value        = points_[(std::size_t)index].value;
    return kResultOk;
}

tresult PLUGIN_API twVst3ParamValueQueue::addPoint( int32 sampleOffset, Vst::ParamValue value,
                                                    int32 &index )
{
    // Capacity was fixed in prepare(). Past it, coalesce onto the last point
    // rather than allocating on the render path: parameters are last-value-wins
    // within a block, so the audible result is the same.
    if( points_.size() >= points_.capacity() && !points_.empty() ) {
        points_.back().offset = sampleOffset;
        points_.back().value  = value;
        index = (int32)( points_.size() - 1 );
        return kResultOk;
    }
    points_.push_back( Point{ sampleOffset, value } );
    index = (int32)( points_.size() - 1 );
    return kResultOk;
}

// --- twVst3EventList (proposal 37 P2) -----------------------------------------

tresult PLUGIN_API twVst3EventList::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, Vst::IEventList::iid );
}

tresult PLUGIN_API twVst3EventList::getEvent( int32 index, Vst::Event &e )
{
    if( index < 0 || (std::size_t)index >= used_ ) return kInvalidArgument;
    e = events_[(std::size_t)index];
    return kResultOk;
}

tresult PLUGIN_API twVst3EventList::addEvent( Vst::Event &e )
{
    // Called by the PLUGIN, on the output list. Overflow is counted and
    // dropped: growing here would allocate on the render path, and answering
    // an error would make a well-behaved plugin log or retry every block.
    append( e );
    return kResultOk;
}

bool twVst3EventList::append( const Vst::Event &e )
{
    if( used_ >= events_.size() ) {
        ++dropped_;
        return false;
    }
    events_[used_++] = e;
    return true;
}

// --- twVst3ParamChanges -------------------------------------------------------

tresult PLUGIN_API twVst3ParamChanges::queryInterface( const TUID _iid, void **obj )
{
    return resolveOne( _iid, obj, Vst::IParameterChanges::iid );
}

void twVst3ParamChanges::reserve( std::size_t nParams, std::size_t maxPointsPerParam )
{
    queues_.clear();
    queues_.reserve( nParams );
    for( std::size_t i = 0; i < nParams; ++i ) {
        std::unique_ptr<twVst3ParamValueQueue> q( new twVst3ParamValueQueue() );
        q->reservePoints( maxPointsPerParam ? maxPointsPerParam : 1 );
        queues_.push_back( std::move( q ) );
    }
    used_ = 0;
}

void twVst3ParamChanges::clearAll()
{
    for( std::size_t i = 0; i < used_; ++i ) queues_[i]->clearPoints();
    used_ = 0;
}

Vst::IParamValueQueue *twVst3ParamChanges::getParameterData( int32 index )
{
    if( index < 0 || (std::size_t)index >= used_ ) return nullptr;
    return queues_[(std::size_t)index].get();
}

Vst::IParamValueQueue *twVst3ParamChanges::addParameterData( const Vst::ParamID &id,
                                                             int32 &index )
{
    for( std::size_t i = 0; i < used_; ++i ) {
        if( queues_[i]->getParameterId() == id ) {
            index = (int32)i;
            return queues_[i].get();
        }
    }
    if( used_ >= queues_.size() ) {
        // Should not happen: reserve() is sized from the parameter count. If a
        // plugin invents a parameter id at runtime, refusing is better than
        // allocating in process().
        TW_LOGW( "plugins", "[vst3] parameter-change queue exhausted (%llu); "
                 "dropping an edit for id %u",
                 (unsigned long long)queues_.size(), (unsigned)id );
        index = -1;
        return nullptr;
    }
    twVst3ParamValueQueue *q = queues_[used_].get();
    q->setId( id );
    q->clearPoints();
    index = (int32)used_;
    ++used_;
    return q;
}

}  // namespace audio
