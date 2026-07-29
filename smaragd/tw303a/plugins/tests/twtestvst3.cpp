// twtestvst3 — a real VST3 module built from this repo, so plugins_test can
// exercise the actual VST3 load path without anyone installing a third-party
// plugin (proposal 08 M6). The direct counterpart of plugins/tests/twtestclap.c.
//
// It is C++ rather than C because VST3 is COM-shaped: the ABI IS a C++ vtable.
// It links its OWN copies of the SDK sources and its own IID definitions —
// a module and its host are separate binaries and must not share either.
//
// One class, "TW Test VST3 Gain", 2 in / 2 out, one parameter:
//
//     out[c] = in[c] * gain          gain normalized [0,1], default 1.0 (unity)
//
// Unity by default means an unedited instance is a pass-through, so a test can
// tell "the plugin ran" from "the plugin was bypassed" by editing the parameter
// and watching the level move. The parameter is deliberately reachable ONLY via
// ProcessData::inputParameterChanges, which is what makes this fixture a
// regression test for the single most common VST3 host bug — a host that writes
// IEditController::setParamNormalized and stops there will see NO level change
// here, because this plugin never consults its controller from process().
//
// Shape: a SINGLE component (IComponent + IAudioProcessor + IEditController on
// one object). The split component/controller shape exercises more host code
// (connection points, setComponentState, separate lifecycles) and is covered by
// the manual real-plugin verification, not by this fixture — a gap recorded in
// 08_PLUGIN_HOSTING_EXECUTION.md rather than papered over.

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <cstring>
#include <new>

// The module's own IID definitions. A plugin needs the same ones the host does
// and for the same reason (vst3_pluginterfaces ships no vstinitiids.cpp), but it
// must define them in ITS OWN binary — these are not shared across the boundary.
namespace Steinberg {
DEF_CLASS_IID( Vst::IComponent )
DEF_CLASS_IID( Vst::IAudioProcessor )
DEF_CLASS_IID( Vst::IEditController )
DEF_CLASS_IID( Vst::IParameterChanges )
DEF_CLASS_IID( Vst::IParamValueQueue )
}  // namespace Steinberg

using namespace Steinberg;

namespace {

// Stable class id. Chosen to be readable in a hex dump — the host spells a uid
// as 32 hex digits, so "TWTESTVST3GAIN\0\0" is greppable in a .qxp and in a log.
const TUID kTestGainCid = INLINE_UID( 0x54575445, 0x53545653, 0x54334741, 0x494E0000 );

constexpr uint32 kParamGain = 0;

constexpr uint32 kStateMagic   = 0x54573356;   // 'TW3V'
constexpr int32  kMaxChannels  = 2;

// --- the plugin ---------------------------------------------------------------
//
// One object implements all three interfaces. FUnknown is therefore a repeated
// base, which would make addRef/release/queryInterface ambiguous — a single
// override in the most-derived class resolves that, because one declaration
// overrides the virtual in every base that declares it.
class TestGain final : public Vst::IComponent,
                       public Vst::IAudioProcessor,
                       public Vst::IEditController {
public:
    // --- FUnknown ------------------------------------------------------------
    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        if( !obj ) return kInvalidArgument;
        if( FUnknownPrivate::iidEqual( _iid, FUnknown::iid ) ||
            FUnknownPrivate::iidEqual( _iid, IPluginBase::iid ) ||
            FUnknownPrivate::iidEqual( _iid, Vst::IComponent::iid ) ) {
            *obj = static_cast<Vst::IComponent *>( this );
            addRef();
            return kResultOk;
        }
        if( FUnknownPrivate::iidEqual( _iid, Vst::IAudioProcessor::iid ) ) {
            *obj = static_cast<Vst::IAudioProcessor *>( this );
            addRef();
            return kResultOk;
        }
        if( FUnknownPrivate::iidEqual( _iid, Vst::IEditController::iid ) ) {
            *obj = static_cast<Vst::IEditController *>( this );
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return (uint32)++refs_; }
    uint32 PLUGIN_API release() override
    {
        if( --refs_ <= 0 ) {
            delete this;
            return 0;
        }
        return (uint32)refs_;
    }

    // --- IPluginBase ---------------------------------------------------------
    tresult PLUGIN_API initialize( FUnknown * ) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // --- IComponent ----------------------------------------------------------
    tresult PLUGIN_API getControllerClassId( TUID ) override
    {
        // Single component: the controller is this same object, so there is no
        // separate class to name.
        return kNotImplemented;
    }
    tresult PLUGIN_API setIoMode( Vst::IoMode ) override { return kNotImplemented; }

    int32 PLUGIN_API getBusCount( Vst::MediaType type, Vst::BusDirection ) override
    {
        return type == Vst::kAudio ? 1 : 0;
    }

    tresult PLUGIN_API getBusInfo( Vst::MediaType type, Vst::BusDirection dir,
                                   int32 index, Vst::BusInfo &bus ) override
    {
        if( type != Vst::kAudio || index != 0 ) return kInvalidArgument;
        bus.mediaType    = Vst::kAudio;
        bus.direction    = dir;
        bus.channelCount = channels_;
        bus.busType      = Vst::kMain;
        bus.flags        = Vst::BusInfo::kDefaultActive;
        copyName( bus.name, dir == Vst::kInput ? "In" : "Out" );
        return kResultOk;
    }

    tresult PLUGIN_API getRoutingInfo( Vst::RoutingInfo &, Vst::RoutingInfo & ) override
    {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus( Vst::MediaType, Vst::BusDirection, int32, TBool ) override
    {
        return kResultOk;
    }
    tresult PLUGIN_API setActive( TBool state ) override
    {
        active_ = state != 0;
        return kResultOk;
    }

    tresult PLUGIN_API setState( IBStream *state ) override
    {
        if( !state ) return kInvalidArgument;
        uint32 magic = 0;
        double gain  = 1.0;
        int32  got   = 0;
        if( state->read( &magic, (int32)sizeof( magic ), &got ) != kResultOk ||
            got != (int32)sizeof( magic ) || magic != kStateMagic )
            return kResultFalse;
        if( state->read( &gain, (int32)sizeof( gain ), &got ) != kResultOk ||
            got != (int32)sizeof( gain ) )
            return kResultFalse;
        gain_ = clamp01( gain );
        return kResultOk;
    }

    tresult PLUGIN_API getState( IBStream *state ) override
    {
        if( !state ) return kInvalidArgument;
        uint32 magic = kStateMagic;
        double gain  = gain_;
        int32  put   = 0;
        if( state->write( &magic, (int32)sizeof( magic ), &put ) != kResultOk )
            return kResultFalse;
        if( state->write( &gain, (int32)sizeof( gain ), &put ) != kResultOk )
            return kResultFalse;
        return kResultOk;
    }

    // --- IAudioProcessor -----------------------------------------------------
    tresult PLUGIN_API setBusArrangements( Vst::SpeakerArrangement *inputs, int32 numIns,
                                           Vst::SpeakerArrangement *outputs,
                                           int32 numOuts ) override
    {
        if( numIns != 1 || numOuts != 1 || !inputs || !outputs ) return kResultFalse;
        if( inputs[0] != outputs[0] ) return kResultFalse;
        const int32 ch = countChannels( inputs[0] );
        if( ch < 1 || ch > kMaxChannels ) return kResultFalse;
        channels_ = ch;
        return kResultOk;
    }

    tresult PLUGIN_API getBusArrangement( Vst::BusDirection, int32 index,
                                          Vst::SpeakerArrangement &arr ) override
    {
        if( index != 0 ) return kInvalidArgument;
        arr = channels_ == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize( int32 symbolicSampleSize ) override
    {
        return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }

    tresult PLUGIN_API setupProcessing( Vst::ProcessSetup &setup ) override
    {
        if( setup.symbolicSampleSize != Vst::kSample32 ) return kResultFalse;
        maxBlock_ = setup.maxSamplesPerBlock;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing( TBool state ) override
    {
        processing_ = state != 0;
        return kResultOk;
    }

    tresult PLUGIN_API process( Vst::ProcessData &data ) override
    {
        // Parameter changes arrive HERE and nowhere else. A host that only wrote
        // the controller sends no queue, gain_ never moves, and the level test
        // fails — which is the whole point of this fixture.
        if( data.inputParameterChanges ) {
            const int32 nq = data.inputParameterChanges->getParameterCount();
            for( int32 q = 0; q < nq; ++q ) {
                Vst::IParamValueQueue *queue = data.inputParameterChanges->getParameterData( q );
                if( !queue || queue->getParameterId() != kParamGain ) continue;
                const int32 nPoints = queue->getPointCount();
                if( nPoints <= 0 ) continue;
                // Last point wins: no sample-accurate ramping in a test fixture.
                int32            offset = 0;
                Vst::ParamValue  value  = gain_;
                if( queue->getPoint( nPoints - 1, offset, value ) == kResultOk )
                    gain_ = clamp01( value );
            }
        }

        if( data.numSamples <= 0 ) return kResultOk;
        if( !data.inputs || !data.outputs || data.numInputs < 1 || data.numOutputs < 1 )
            return kResultOk;
        if( data.symbolicSampleSize != Vst::kSample32 ) return kResultFalse;

        const Vst::AudioBusBuffers &in  = data.inputs[0];
        Vst::AudioBusBuffers       &out = data.outputs[0];
        const float                 g   = (float)gain_;

        for( int32 c = 0; c < out.numChannels; ++c ) {
            float *o = out.channelBuffers32 ? out.channelBuffers32[c] : nullptr;
            if( !o ) continue;
            const float *i = ( c < in.numChannels && in.channelBuffers32 )
                                 ? in.channelBuffers32[c]
                                 : nullptr;
            if( i ) {
                for( int32 s = 0; s < data.numSamples; ++s ) o[s] = i[s] * g;
            } else {
                std::memset( o, 0, (std::size_t)data.numSamples * sizeof( float ) );
            }
        }
        out.silenceFlags = 0;
        return kResultOk;
    }

    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // --- IEditController -----------------------------------------------------
    tresult PLUGIN_API setComponentState( IBStream *state ) override
    {
        return setState( state );
    }
    int32 PLUGIN_API getParameterCount() override { return 1; }

    tresult PLUGIN_API getParameterInfo( int32 paramIndex, Vst::ParameterInfo &info ) override
    {
        if( paramIndex != 0 ) return kInvalidArgument;
        std::memset( &info, 0, sizeof( info ) );
        info.id                     = kParamGain;
        info.stepCount              = 0;
        info.defaultNormalizedValue = 1.0;
        info.unitId                 = 0;   // kRootUnitId, without pulling in ivstunits.h
        info.flags                  = Vst::ParameterInfo::kCanAutomate;
        copyName( info.title, "Gain" );
        copyName( info.shortTitle, "Gain" );
        return kResultOk;
    }

    tresult PLUGIN_API getParamStringByValue( Vst::ParamID, Vst::ParamValue,
                                              Vst::String128 ) override
    {
        return kNotImplemented;
    }
    tresult PLUGIN_API getParamValueByString( Vst::ParamID, Vst::TChar *,
                                              Vst::ParamValue & ) override
    {
        return kNotImplemented;
    }
    Vst::ParamValue PLUGIN_API normalizedParamToPlain( Vst::ParamID, Vst::ParamValue v ) override
    {
        return v;
    }
    Vst::ParamValue PLUGIN_API plainParamToNormalized( Vst::ParamID, Vst::ParamValue v ) override
    {
        return v;
    }
    Vst::ParamValue PLUGIN_API getParamNormalized( Vst::ParamID id ) override
    {
        return id == kParamGain ? gain_ : 0.0;
    }
    tresult PLUGIN_API setParamNormalized( Vst::ParamID id, Vst::ParamValue value ) override
    {
        // Deliberately does NOT change what process() uses. The controller and
        // the processor are the same object here, so honouring this would hide
        // exactly the host bug the fixture exists to catch.
        if( id != kParamGain ) return kInvalidArgument;
        controllerGain_ = clamp01( value );
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler( Vst::IComponentHandler * ) override
    {
        return kResultOk;
    }
    IPlugView *PLUGIN_API createView( FIDString ) override { return nullptr; }

private:
    ~TestGain() = default;

    static double clamp01( double v ) { return v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v ); }

    static int32 countChannels( Vst::SpeakerArrangement a )
    {
        int32 n = 0;
        for( int i = 0; i < 64; ++i )
            if( a & ( (Vst::SpeakerArrangement)1 << i ) ) ++n;
        return n;
    }

    static void copyName( Vst::String128 dst, const char *src )
    {
        std::size_t i = 0;
        for( ; src[i] && i < 127; ++i ) dst[i] = (Vst::TChar)(unsigned char)src[i];
        dst[i] = 0;
    }

    int32  refs_           = 1;
    int32  channels_       = 2;
    int32  maxBlock_       = 0;
    bool   active_         = false;
    bool   processing_     = false;
    double gain_           = 1.0;   // what process() uses
    double controllerGain_ = 1.0;   // what setParamNormalized wrote; unused on purpose
};

// --- factory ------------------------------------------------------------------

class TestFactory final : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        if( !obj ) return kInvalidArgument;
        if( FUnknownPrivate::iidEqual( _iid, FUnknown::iid ) ||
            FUnknownPrivate::iidEqual( _iid, IPluginFactory::iid ) ||
            FUnknownPrivate::iidEqual( _iid, IPluginFactory2::iid ) ) {
            *obj = static_cast<IPluginFactory2 *>( this );
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // The factory is a file-scope singleton: refcounting it is bookkeeping only.
    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    tresult PLUGIN_API getFactoryInfo( PFactoryInfo *info ) override
    {
        if( !info ) return kInvalidArgument;
        // Value-init, not memset: these structs have real constructors, and
        // memset over a non-trivial type is exactly what -Wclass-memaccess is for.
        *info = PFactoryInfo();
        std::strncpy( info->vendor, "Smaragd", PFactoryInfo::kNameSize - 1 );
        std::strncpy( info->url, "https://github.com/tweggen/qbx", PFactoryInfo::kURLSize - 1 );
        std::strncpy( info->email, "none", PFactoryInfo::kEmailSize - 1 );
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }

    int32 PLUGIN_API countClasses() override { return 1; }

    tresult PLUGIN_API getClassInfo( int32 index, PClassInfo *info ) override
    {
        if( index != 0 || !info ) return kInvalidArgument;
        *info = PClassInfo();
        std::memcpy( info->cid, kTestGainCid, sizeof( TUID ) );
        info->cardinality = PClassInfo::kManyInstances;
        std::strncpy( info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1 );
        std::strncpy( info->name, "TW Test VST3 Gain", PClassInfo::kNameSize - 1 );
        return kResultOk;
    }

    tresult PLUGIN_API getClassInfo2( int32 index, PClassInfo2 *info ) override
    {
        if( index != 0 || !info ) return kInvalidArgument;
        *info = PClassInfo2();
        std::memcpy( info->cid, kTestGainCid, sizeof( TUID ) );
        info->cardinality = PClassInfo::kManyInstances;
        std::strncpy( info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1 );
        std::strncpy( info->name, "TW Test VST3 Gain", PClassInfo::kNameSize - 1 );
        info->classFlags = 0;
        std::strncpy( info->subCategories, "Fx", PClassInfo2::kSubCategoriesSize - 1 );
        std::strncpy( info->vendor, "Smaragd", PClassInfo2::kVendorSize - 1 );
        std::strncpy( info->version, "1.0.0", PClassInfo2::kVersionSize - 1 );
        std::strncpy( info->sdkVersion, "VST 3.7", PClassInfo2::kVersionSize - 1 );
        return kResultOk;
    }

    tresult PLUGIN_API createInstance( FIDString cid, FIDString _iid, void **obj ) override
    {
        if( !cid || !_iid || !obj ) return kInvalidArgument;
        *obj = nullptr;
        if( !FUnknownPrivate::iidEqual( cid, kTestGainCid ) ) return kNoInterface;

        TestGain *p = new( std::nothrow ) TestGain();
        if( !p ) return kOutOfMemory;
        // FIDString and `const TUID` are both const char* after decay; no cast.
        const tresult r = p->queryInterface( _iid, obj );
        p->release();   // the QI above took the reference the caller keeps
        return r;
    }
};

TestFactory gFactory;

}  // namespace

// --- module entry points ------------------------------------------------------

#if defined( _WIN32 )
#define TWTEST_EXPORT extern "C" __declspec( dllexport )
#else
#define TWTEST_EXPORT extern "C" __attribute__( ( visibility( "default" ) ) )
#endif

#if defined( _WIN32 )
TWTEST_EXPORT bool InitDll() { return true; }
TWTEST_EXPORT bool ExitDll() { return true; }
#elif defined( __APPLE__ )
TWTEST_EXPORT bool bundleEntry( void * ) { return true; }
TWTEST_EXPORT bool bundleExit() { return true; }
#else
TWTEST_EXPORT bool ModuleEntry( void * ) { return true; }
TWTEST_EXPORT bool ModuleExit() { return true; }
#endif

TWTEST_EXPORT IPluginFactory *GetPluginFactory()
{
    return &gFactory;
}
