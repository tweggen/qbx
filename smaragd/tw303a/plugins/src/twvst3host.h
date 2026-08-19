#ifndef _TWVST3HOST_H_
#define _TWVST3HOST_H_

// PRIVATE header of the VST3 backend (proposal 08 M6) — the host-side objects a
// VST3 plugin calls back into.
//
// Like twclapmodule.h this deliberately does NOT live under
// plugins/include/tw/plugins/: only files inside tw_plugins may see
// <pluginterfaces/...>, because that include directory and TW_HAVE_VST3 are both
// PRIVATE to the target. A public header that changed shape with TW_HAVE_VST3
// would give every other module a different view of the same types (ODR/ABI
// skew) — CONTRACT invariant 4.
//
// These translation units are only compiled when the vst3_pluginterfaces
// submodule is present (see the VST3 block in tw303a/CMakeLists.txt), so there
// is no #ifdef inside them.
//
// OWNERSHIP, which is the only subtle thing here
// ----------------------------------------------
// VST3 is COM-shaped, so every object crossing the boundary is refcounted — but
// the two directions have opposite lifetimes, and conflating them is how a host
// gets a use-after-free that only reproduces on someone else's plugin:
//
//   * BORROWED objects (twVst3Borrowed) are ours, live as long as the plugin
//     instance that borrows them, and NEVER self-delete on release(). The host
//     context, the parameter-change queues and the memory streams are all of
//     this kind. A plugin that over-releases one of these is a bug we survive.
//   * OWNED objects (twVst3Owned) are manufactured on demand — IMessage and its
//     IAttributeList — handed to the plugin, and destroyed when the last
//     release() lands. The plugin decides when that is.
//
// Nothing in here touches Qt or allocates on the audio thread except where
// noted; twVst3ParamChanges is explicitly pre-sized so process() does not.

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"

// twEditorGesture: the phase a recorded edit carries. The editor ABI is
// format-neutral and public, and this private backend header quotes it rather
// than inventing a second Begin/Change/End enum that would have to be mapped.
#include "tw/plugins/twplugineditor.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace audio {

// --- refcount bases -----------------------------------------------------------

// Ours, borrowed by the plugin. release() never destroys.
template <class Iface>
class twVst3Borrowed : public Iface {
public:
    Steinberg::uint32 PLUGIN_API addRef() override
    {
        return (Steinberg::uint32)++refs_;
    }
    Steinberg::uint32 PLUGIN_API release() override
    {
        // Clamped at zero rather than allowed to wrap: an over-releasing plugin
        // must not turn into an unsigned underflow that later reads as "alive".
        std::int32_t cur = refs_.load( std::memory_order_relaxed );
        while( cur > 0 && !refs_.compare_exchange_weak( cur, cur - 1 ) ) {}
        return (Steinberg::uint32)( cur > 0 ? cur - 1 : 0 );
    }

protected:
    // The FUnknown leg, shared by every subclass; each adds its own iid.
    Steinberg::tresult resolveOne( const Steinberg::TUID _iid, void **obj,
                                   const Steinberg::FUID &mine )
    {
        if( Steinberg::FUnknownPrivate::iidEqual( _iid, Steinberg::FUnknown::iid ) ||
            Steinberg::FUnknownPrivate::iidEqual( _iid, mine ) ) {
            *obj = this;
            this->addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

private:
    std::atomic<std::int32_t> refs_{ 1 };
};

// Manufactured for the plugin; destroyed when the plugin lets go.
template <class Iface>
class twVst3Owned : public Iface {
public:
    Steinberg::uint32 PLUGIN_API addRef() override
    {
        return (Steinberg::uint32)++refs_;
    }
    Steinberg::uint32 PLUGIN_API release() override
    {
        const std::int32_t n = --refs_;
        if( n <= 0 ) {
            delete this;
            return 0;
        }
        return (Steinberg::uint32)n;
    }

protected:
    virtual ~twVst3Owned() = default;

    Steinberg::tresult resolveOne( const Steinberg::TUID _iid, void **obj,
                                   const Steinberg::FUID &mine )
    {
        if( Steinberg::FUnknownPrivate::iidEqual( _iid, Steinberg::FUnknown::iid ) ||
            Steinberg::FUnknownPrivate::iidEqual( _iid, mine ) ) {
            *obj = this;
            this->addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

private:
    std::atomic<std::int32_t> refs_{ 1 };
};

// --- IBStream over a byte vector ----------------------------------------------

// The carrier for IComponent/IEditController getState/setState. Borrowed: it is
// always a local of the call that drives it.
class twVst3MemStream final : public twVst3Borrowed<Steinberg::IBStream> {
public:
    twVst3MemStream() = default;
    explicit twVst3MemStream( std::vector<std::uint8_t> initial )
        : data_( std::move( initial ) ) {}

    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;
    Steinberg::tresult PLUGIN_API read( void *buffer, Steinberg::int32 numBytes,
                                        Steinberg::int32 *numBytesRead ) override;
    Steinberg::tresult PLUGIN_API write( void *buffer, Steinberg::int32 numBytes,
                                         Steinberg::int32 *numBytesWritten ) override;
    Steinberg::tresult PLUGIN_API seek( Steinberg::int64 pos, Steinberg::int32 mode,
                                        Steinberg::int64 *result ) override;
    Steinberg::tresult PLUGIN_API tell( Steinberg::int64 *pos ) override;

    void rewind() { pos_ = 0; }
    const std::vector<std::uint8_t> &bytes() const { return data_; }

private:
    std::vector<std::uint8_t> data_;
    Steinberg::int64          pos_ = 0;
};

// --- IAttributeList / IMessage ------------------------------------------------

// A split component/controller pair talks to itself THROUGH the host: it asks
// IHostApplication for an IMessage, fills its attribute list, and pushes it at
// its IConnectionPoint peer. A host that answers "kNotImplemented" to
// createInstance loads such a plugin but silently breaks that channel, which is
// why these are real implementations and not stubs.
class twVst3AttributeList final : public twVst3Owned<Steinberg::Vst::IAttributeList> {
public:
    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;

    Steinberg::tresult PLUGIN_API setInt( AttrID id, Steinberg::int64 value ) override;
    Steinberg::tresult PLUGIN_API getInt( AttrID id, Steinberg::int64 &value ) override;
    Steinberg::tresult PLUGIN_API setFloat( AttrID id, double value ) override;
    Steinberg::tresult PLUGIN_API getFloat( AttrID id, double &value ) override;
    Steinberg::tresult PLUGIN_API setString( AttrID id,
                                             const Steinberg::Vst::TChar *string ) override;
    Steinberg::tresult PLUGIN_API getString( AttrID id, Steinberg::Vst::TChar *string,
                                             Steinberg::uint32 sizeInBytes ) override;
    Steinberg::tresult PLUGIN_API setBinary( AttrID id, const void *data,
                                             Steinberg::uint32 sizeInBytes ) override;
    Steinberg::tresult PLUGIN_API getBinary( AttrID id, const void *&data,
                                             Steinberg::uint32 &sizeInBytes ) override;

private:
    enum class Kind { Int, Float, String, Binary };
    struct Value {
        Kind                      kind = Kind::Int;
        Steinberg::int64          i    = 0;
        double                    f    = 0.0;
        std::vector<std::uint8_t> bytes;   // String: UTF-16 code units; Binary: raw
    };
    std::map<std::string, Value> values_;
};

class twVst3Message final : public twVst3Owned<Steinberg::Vst::IMessage> {
public:
    twVst3Message();

    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;

    Steinberg::FIDString PLUGIN_API getMessageID() override;
    void PLUGIN_API                 setMessageID( Steinberg::FIDString id ) override;
    Steinberg::Vst::IAttributeList *PLUGIN_API getAttributes() override;

private:
    ~twVst3Message() override;

    std::string           id_;
    twVst3AttributeList  *attrs_ = nullptr;   // owned; released in the dtor
};

// --- IPlugInterfaceSupport / IHostApplication ---------------------------------

class twVst3PlugInterfaceSupport final
    : public twVst3Borrowed<Steinberg::Vst::IPlugInterfaceSupport> {
public:
    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;
    Steinberg::tresult PLUGIN_API isPlugInterfaceSupported(
        const Steinberg::TUID _iid ) override;
};

// The object handed to IPluginBase::initialize(). One per plugin instance; it
// outlives every call the plugin makes on it because the instance owns it.
class twVst3HostApplication final
    : public twVst3Borrowed<Steinberg::Vst::IHostApplication> {
public:
    explicit twVst3HostApplication( std::string instanceName );

    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;
    Steinberg::tresult PLUGIN_API getName( Steinberg::Vst::String128 name ) override;
    Steinberg::tresult PLUGIN_API createInstance( Steinberg::TUID cid,
                                                  Steinberg::TUID _iid,
                                                  void **obj ) override;

private:
    std::string                 name_;
    twVst3PlugInterfaceSupport  support_;
};

// --- IComponentHandler --------------------------------------------------------

// The plugin reports its own parameter edits here (a knob turned in a native
// editor, or an internal change). Recorded only: this may be called from the
// plugin's UI thread, and reaching into the graph from it would violate the
// no-Qt-off-the-main-thread rule this repo has already been bitten by. The
// backend polls the flags.
class twVst3ComponentHandler final
    : public twVst3Borrowed<Steinberg::Vst::IComponentHandler> {
public:
    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;

    Steinberg::tresult PLUGIN_API beginEdit( Steinberg::Vst::ParamID id ) override;
    Steinberg::tresult PLUGIN_API performEdit( Steinberg::Vst::ParamID id,
                                               Steinberg::Vst::ParamValue v ) override;
    Steinberg::tresult PLUGIN_API endEdit( Steinberg::Vst::ParamID id ) override;
    Steinberg::tresult PLUGIN_API restartComponent( Steinberg::int32 flags ) override;

    // Set when the plugin changed a parameter behind our back; the backend
    // refreshes its mirror on the next main-thread touch.
    bool takeEditFlag() { return edited_.exchange( false, std::memory_order_acq_rel ); }
    bool takeRestartFlag() { return restart_.exchange( false, std::memory_order_acq_rel ); }

    // --- what the plugin's own GUI actually did (proposal 33 M2) -------------
    //
    // Until M2 this class took (id, value) and THREW BOTH AWAY — the parameters
    // of performEdit were literally unnamed — leaving a bare "something changed"
    // bit that nothing ever read. That was survivable only while no native
    // editor existed: a knob turned in the plugin's own UI produced a flag with
    // no id, no value and no reader, so the sound could not follow it.
    //
    // The queue is a plain vector under a plain mutex, NOT a lock-free ring, and
    // that is a considered choice: these are UI-rate events (a drag is tens per
    // second, not thousands), the audio thread never touches this object, and a
    // ring would need a fixed capacity whose overflow policy would silently drop
    // the END of a gesture — the one event that must never be lost, because it
    // is what closes an undo entry and an automation punch-in.
    //
    // The VST3 spec says IComponentHandler is called on the UI thread; real
    // plugins are not uniformly careful about that, so the lock is also what
    // makes a misbehaving one merely slow instead of a data race.
    //
    // AND THE LOCK IS SAFE EVEN THEN, which is the part worth checking rather
    // than assuming: the worst case is a plugin calling performEdit from inside
    // process(), and in this engine process() NEVER runs on the RT audio
    // callback — twRtThreadGuard refuses a freeze on that thread outright, and
    // the RT path only reads ready pages (proposal 19). So such a call lands on
    // a freeze worker or the live pump, where a short uncontended mutex is
    // ordinary. There is no path by which this lock can be taken on the audio
    // callback, and that is a property of the engine's design rather than of
    // plugins behaving.
    struct Edit {
        std::uint32_t                     id    = 0;
        double                            value = 0.0;   // normalized [0,1]
        twEditorGesture                   phase = twEditorGesture::Change;
    };

    // Hand over everything since the last call and clear. Main thread.
    std::vector<Edit> takeEdits();

    // restartComponent's flags, which used to be discarded outright
    // (`(void)flags;`). kParamValuesChanged and kLatencyChanged mean different
    // things to the host and were indistinguishable; the backend needs them
    // separated to decide between "re-read the values" and "re-read everything".
    Steinberg::int32 takeRestartFlags()
    { return restartFlags_.exchange( 0, std::memory_order_acq_rel ); }

private:
    void record_( Steinberg::Vst::ParamID id, double value, twEditorGesture phase );

    std::atomic<bool> edited_{ false };
    std::atomic<bool> restart_{ false };

    std::mutex          editMutex_;
    std::vector<Edit>   edits_;
    std::atomic<Steinberg::int32> restartFlags_{ 0 };

    // A drag that outlives its reader — the host stopped draining, or a plugin
    // streams continuous values with no end in sight — must not grow without
    // bound. Dropping the OLDEST intermediate value is safe in a way dropping
    // the newest is not: the newest is the current position of the control, and
    // Begin/End are never dropped (see the .cc).
    static constexpr std::size_t kMaxPendingEdits = 4096;
};

// --- IParameterChanges --------------------------------------------------------

// The ONLY route a parameter edit can take to the processor. Writing
// IEditController::setParamNormalized alone updates the editor's idea of the
// value and never reaches the DSP — the single most common VST3 host bug, and
// what 08_PLUGIN_HOSTING_EXECUTION.md M6 calls out explicitly.
//
// Borrowed, and pre-sized in prepare(): process() only ever rewinds and refills
// it, so no allocation happens on the render path.
class twVst3ParamValueQueue final
    : public twVst3Borrowed<Steinberg::Vst::IParamValueQueue> {
public:
    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    Steinberg::int32 PLUGIN_API        getPointCount() override
    {
        return (Steinberg::int32)points_.size();
    }
    Steinberg::tresult PLUGIN_API getPoint( Steinberg::int32 index,
                                            Steinberg::int32 &sampleOffset,
                                            Steinberg::Vst::ParamValue &value ) override;
    Steinberg::tresult PLUGIN_API addPoint( Steinberg::int32 sampleOffset,
                                            Steinberg::Vst::ParamValue value,
                                            Steinberg::int32 &index ) override;

    void setId( Steinberg::Vst::ParamID id ) { id_ = id; }
    void clearPoints() { points_.clear(); }
    void reservePoints( std::size_t n ) { points_.reserve( n ); }

private:
    struct Point {
        Steinberg::int32           offset;
        Steinberg::Vst::ParamValue value;
    };
    Steinberg::Vst::ParamID id_ = 0;
    std::vector<Point>      points_;
};

// --- IEventList (proposal 37 P2) ----------------------------------------------
//
// The note lane, in both directions. Borrowed and pre-sized in prepare(), for
// the same reason twVst3ParamChanges is: process() must not allocate.
//
// VST3 events are a flat POD struct with a type tag and a union, so ONE vector
// holds a mixed sequence with no slicing and no per-type storage.
class twVst3EventList final : public twVst3Borrowed<Steinberg::Vst::IEventList> {
public:
    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;

    Steinberg::int32 PLUGIN_API getEventCount() override
    {
        return (Steinberg::int32)used_;
    }
    Steinberg::tresult PLUGIN_API getEvent( Steinberg::int32              index,
                                            Steinberg::Vst::Event &e ) override;
    // The plugin's own output lands here. Beyond the reserved capacity it is
    // COUNTED AND DROPPED, never grown (no allocation on the render path) and
    // never refused with an error — a chatty arpeggiator must not fail a render.
    Steinberg::tresult PLUGIN_API addEvent( Steinberg::Vst::Event &e ) override;

    void reserve( std::size_t n ) { events_.resize( n ); }
    void clear()
    {
        used_    = 0;
        dropped_ = 0;
    }
    bool        append( const Steinberg::Vst::Event &e );
    std::size_t size() const { return used_; }
    std::size_t dropped() const { return dropped_; }
    const Steinberg::Vst::Event &at( std::size_t i ) const { return events_[i]; }

private:
    std::vector<Steinberg::Vst::Event> events_;
    std::size_t                        used_    = 0;
    std::size_t                        dropped_ = 0;
};

class twVst3ParamChanges final : public twVst3Borrowed<Steinberg::Vst::IParameterChanges> {
public:
    Steinberg::tresult PLUGIN_API queryInterface( const Steinberg::TUID _iid,
                                                  void **obj ) override;

    Steinberg::int32 PLUGIN_API getParameterCount() override
    {
        return (Steinberg::int32)used_;
    }
    Steinberg::Vst::IParamValueQueue *PLUGIN_API getParameterData(
        Steinberg::int32 index ) override;
    Steinberg::Vst::IParamValueQueue *PLUGIN_API addParameterData(
        const Steinberg::Vst::ParamID &id, Steinberg::int32 &index ) override;

    // Called from prepare(): fixes the capacity so the render path never grows
    // the vector. maxPointsPerParam is how many edits one parameter may carry in
    // a single block before the surplus coalesces onto the last point.
    void reserve( std::size_t nParams, std::size_t maxPointsPerParam );
    void clearAll();

private:
    std::vector<std::unique_ptr<twVst3ParamValueQueue>> queues_;
    std::size_t                                         used_ = 0;
};

}  // namespace audio

#endif
