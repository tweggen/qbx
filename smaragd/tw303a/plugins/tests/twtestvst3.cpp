// twtestvst3 — a real VST3 module built from this repo, so plugins_test can
// exercise the actual VST3 load path without anyone installing a third-party
// plugin (proposal 08 M6). The direct counterpart of plugins/tests/twtestclap.c.
//
// It is C++ rather than C because VST3 is COM-shaped: the ABI IS a C++ vtable.
// It links its OWN copies of the SDK sources and its own IID definitions —
// a module and its host are separate binaries and must not share either.
//
// TWO classes since proposal 36 P2:
//
//   "TW Test VST3 Gain"  2 in / 2 out effect, SINGLE component     (M6)
//   "TW Test VST3 Sine"  0 in / stereo out INSTRUMENT, SPLIT pair  (36 P2)
//
// The split pair closes the "split VST3 component/controller untested" debt
// plugins/CONTRACT.md has carried since M6 — and it had to, because IMidiMapping
// and the note-expression declaration live on the CONTROLLER, so a host that
// only ever talked to single-component plugins had never exercised the path at
// all. See the TestSine block near the bottom of this file.
//
// --- "TW Test VST3 Gain", 2 in / 2 out, one parameter: ---
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
// one object). The SPLIT shape — connection points, getControllerClassId,
// setComponentState, a separate controller lifecycle — is TestSine's job,
// further down this file.

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <cmath>
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
// proposal 36 P2: the SPLIT pair (TestSine) needs these in its OWN binary.
DEF_CLASS_IID( Vst::IEventList )
DEF_CLASS_IID( Vst::IConnectionPoint )
DEF_CLASS_IID( Vst::IMidiMapping )
DEF_CLASS_IID( Vst::IMessage )
DEF_CLASS_IID( Vst::IAttributeList )
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
        if( data.numSamples <= 0 ) {
            applyParamsUpTo( data, data.numSamples );
            return kResultOk;
        }
        if( !data.inputs || !data.outputs || data.numInputs < 1 || data.numOutputs < 1 )
            return kResultOk;
        if( data.symbolicSampleSize != Vst::kSample32 ) return kResultFalse;

        const Vst::AudioBusBuffers &in  = data.inputs[0];
        Vst::AudioBusBuffers       &out = data.outputs[0];

        // SAMPLE-ACCURATE parameter points (proposal 36 AC2). The block is
        // rendered in SEGMENTS split at each point's sampleOffset, so a gain
        // written at offset 1234 takes effect at frame 1234 and not at the top
        // of the block. A host that passes offset 0 for everything — which is
        // what the pre-36 parameter ring did — gets one segment and exactly the
        // arithmetic M6 had.
        //
        // Parameter changes still arrive HERE and nowhere else: a host that only
        // wrote the controller sends no queue, gain_ never moves, and the level
        // assertion fails. That is the whole point of this fixture.
        // A point AT offset 0 belongs before the first sample.
        applyParamsAt( data, 0 );

        int32 pos = 0;
        for( ;; ) {
            const int32 next = nextParamOffset( data, pos );
            const int32 to   = next < 0 ? data.numSamples : next;
            renderRange( in, out, pos, to );
            if( next < 0 ) break;
            applyParamsAt( data, next );
            pos = next;
            if( pos >= data.numSamples ) break;
        }

        out.silenceFlags = 0;
        return kResultOk;
    }

    // The smallest point offset strictly greater than `after`, or -1.
    int32 nextParamOffset( Vst::ProcessData &data, int32 after )
    {
        if( !data.inputParameterChanges ) return -1;
        int32 best = -1;
        const int32 nq = data.inputParameterChanges->getParameterCount();
        for( int32 q = 0; q < nq; ++q ) {
            Vst::IParamValueQueue *queue = data.inputParameterChanges->getParameterData( q );
            if( !queue || queue->getParameterId() != kParamGain ) continue;
            const int32 nPoints = queue->getPointCount();
            for( int32 pi = 0; pi < nPoints; ++pi ) {
                int32           off = 0;
                Vst::ParamValue v   = 0.0;
                if( queue->getPoint( pi, off, v ) != kResultOk ) continue;
                if( off < 0 ) off = 0;
                if( off > data.numSamples ) off = data.numSamples;
                if( off > after && ( best < 0 || off < best ) ) best = off;
            }
        }
        return best;
    }

    void applyParamsAt( Vst::ProcessData &data, int32 at )
    {
        if( !data.inputParameterChanges ) return;
        const int32 nq = data.inputParameterChanges->getParameterCount();
        for( int32 q = 0; q < nq; ++q ) {
            Vst::IParamValueQueue *queue = data.inputParameterChanges->getParameterData( q );
            if( !queue || queue->getParameterId() != kParamGain ) continue;
            const int32 nPoints = queue->getPointCount();
            for( int32 pi = 0; pi < nPoints; ++pi ) {
                int32           off = 0;
                Vst::ParamValue v   = 0.0;
                if( queue->getPoint( pi, off, v ) != kResultOk ) continue;
                if( off == at ) gain_ = clamp01( v );
            }
        }
    }

    void applyParamsUpTo( Vst::ProcessData &data, int32 limit )
    {
        if( !data.inputParameterChanges ) return;
        const int32 nq = data.inputParameterChanges->getParameterCount();
        for( int32 q = 0; q < nq; ++q ) {
            Vst::IParamValueQueue *queue = data.inputParameterChanges->getParameterData( q );
            if( !queue || queue->getParameterId() != kParamGain ) continue;
            const int32 nPoints = queue->getPointCount();
            for( int32 pi = 0; pi < nPoints; ++pi ) {
                int32           off = 0;
                Vst::ParamValue v   = 0.0;
                if( queue->getPoint( pi, off, v ) == kResultOk && off <= limit )
                    gain_ = clamp01( v );
            }
        }
    }

    void renderRange( const Vst::AudioBusBuffers &in, Vst::AudioBusBuffers &out,
                      int32 from, int32 to )
    {
        if( to <= from ) return;
        const float g = (float)gain_;
        for( int32 c = 0; c < out.numChannels; ++c ) {
            float *o = out.channelBuffers32 ? out.channelBuffers32[c] : nullptr;
            if( !o ) continue;
            const float *i = ( c < in.numChannels && in.channelBuffers32 )
                                 ? in.channelBuffers32[c]
                                 : nullptr;
            if( i ) {
                for( int32 s = from; s < to; ++s ) o[s] = i[s] * g;
            } else {
                std::memset( o + from, 0, (std::size_t)( to - from ) * sizeof( float ) );
            }
        }
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

// --- TestSine: the SPLIT component/controller instrument (proposal 36 P2) -----
//
// Everything here exists to make a host path fail loudly if it is wrong:
//
//  * SPLIT. IComponent+IAudioProcessor live on one object, IEditController+
//    IMidiMapping on another, reached through getControllerClassId. A host that
//    only ever met single-component plugins never exercised setComponentState,
//    the connection points, or a separate controller lifecycle — the gap
//    plugins/CONTRACT.md recorded as debt at M6, which became load-bearing the
//    moment IMidiMapping mattered.
//  * IT IGNORES AN UNACTIVATED EVENT BUS. activateBus(kEvent, kInput, 0, true)
//    is what the spec requires of a host and what our backend did NOT do until
//    P2. With the bus off, inputEvents is ignored entirely and the plugin is
//    SILENT — so the host regression has teeth (AC3) instead of being invisible.
//  * SAMPLE-ACCURATE GAIN. A parameter point takes effect at its sampleOffset,
//    like the gain fixture.
//  * IMidiMapping maps CC 7 (Channel Volume) to the Gain parameter, which is the
//    only route a control change has in VST3 at all.
//
// 0 in / stereo out; one sine voice per held note (8 of them), amp = velocity,
// no envelope, phase 0 at note-on: the same closed-form and deterministic
// properties as the CLAP sine fixture, for the same reason.

const TUID kTestSineCid     = INLINE_UID( 0x54575445, 0x53545653, 0x54335349, 0x4E450000 );
const TUID kTestSineCtrlCid = INLINE_UID( 0x54575445, 0x53545653, 0x54335343, 0x54524C00 );

constexpr int32  kSineVoices        = 8;
constexpr uint32 kSineStateMagic    = 0x54573353;   // 'TW3S'
constexpr uint32 kSineCtrlStateMagic = 0x54573343;  // 'TW3C'

class TestSineController final : public Vst::IEditController,
                                 public Vst::IMidiMapping,
                                 public Vst::IConnectionPoint {
public:
    tresult PLUGIN_API queryInterface( const TUID _iid, void **obj ) override
    {
        if( !obj ) return kInvalidArgument;
        if( FUnknownPrivate::iidEqual( _iid, FUnknown::iid ) ||
            FUnknownPrivate::iidEqual( _iid, IPluginBase::iid ) ||
            FUnknownPrivate::iidEqual( _iid, Vst::IEditController::iid ) ) {
            *obj = static_cast<Vst::IEditController *>( this );
            addRef();
            return kResultOk;
        }
        if( FUnknownPrivate::iidEqual( _iid, Vst::IMidiMapping::iid ) ) {
            *obj = static_cast<Vst::IMidiMapping *>( this );
            addRef();
            return kResultOk;
        }
        if( FUnknownPrivate::iidEqual( _iid, Vst::IConnectionPoint::iid ) ) {
            *obj = static_cast<Vst::IConnectionPoint *>( this );
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return (uint32)++refs_; }
    uint32 PLUGIN_API release() override
    {
        if( --refs_ <= 0 ) { delete this; return 0; }
        return (uint32)refs_;
    }

    tresult PLUGIN_API initialize( FUnknown * ) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // --- IConnectionPoint (the pair talks to itself through the host) --------
    tresult PLUGIN_API connect( Vst::IConnectionPoint *other ) override
    {
        peer_ = other;
        return kResultOk;
    }
    tresult PLUGIN_API disconnect( Vst::IConnectionPoint * ) override
    {
        peer_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify( Vst::IMessage * ) override { return kResultOk; }

    // --- IEditController -----------------------------------------------------
    tresult PLUGIN_API setComponentState( IBStream *state ) override
    {
        // A separate controller MUST be seeded from the component's state, or
        // the editor shows defaults over restored audio (CONTRACT invariant 24).
        if( !state ) return kInvalidArgument;
        uint32 magic = 0;
        double gain  = 1.0;
        int32  got   = 0;
        if( state->read( &magic, (int32)sizeof( magic ), &got ) != kResultOk ||
            got != (int32)sizeof( magic ) || magic != kSineStateMagic )
            return kResultFalse;
        if( state->read( &gain, (int32)sizeof( gain ), &got ) != kResultOk ||
            got != (int32)sizeof( gain ) )
            return kResultFalse;
        gain_ = clamp01( gain );
        return kResultOk;
    }
    tresult PLUGIN_API setState( IBStream * ) override { return kResultOk; }
    tresult PLUGIN_API getState( IBStream *state ) override
    {
        // The controller's OWN chunk, distinct from the component's, so the
        // host's two-chunk frame has something to carry in its second slot —
        // which a single-component plugin structurally cannot provide.
        if( !state ) return kInvalidArgument;
        uint32 magic = kSineCtrlStateMagic;
        int32  put   = 0;
        if( state->write( &magic, (int32)sizeof( magic ), &put ) != kResultOk )
            return kResultFalse;
        return kResultOk;
    }

    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo( int32 index, Vst::ParameterInfo &info ) override
    {
        if( index != 0 ) return kInvalidArgument;
        std::memset( &info, 0, sizeof( info ) );
        info.id                     = kParamGain;
        info.stepCount              = 0;
        info.defaultNormalizedValue = 1.0;
        info.unitId                 = 0;
        info.flags                  = Vst::ParameterInfo::kCanAutomate;
        copyName2( info.title, "Gain" );
        copyName2( info.shortTitle, "Gain" );
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
    tresult PLUGIN_API setParamNormalized( Vst::ParamID id, Vst::ParamValue v ) override
    {
        // Deliberately does NOT reach the processor — the same trap as TestGain,
        // and in a SPLIT plugin it genuinely cannot, which is the honest shape.
        if( id != kParamGain ) return kInvalidArgument;
        gain_ = clamp01( v );
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler( Vst::IComponentHandler * ) override
    {
        return kResultOk;
    }
    IPlugView *PLUGIN_API createView( FIDString ) override { return nullptr; }

    // --- IMidiMapping --------------------------------------------------------
    // VST3 has no control-change EVENT at all; this map is the only route a CC
    // has to the DSP (proposal 36 §5.2).
    tresult PLUGIN_API getMidiControllerAssignment( int32 busIndex, int16 channel,
                                                    Vst::CtrlNumber midiControllerNumber,
                                                    Vst::ParamID &id ) override
    {
        (void)channel;
        if( busIndex != 0 ) return kResultFalse;
        if( midiControllerNumber != Vst::kCtrlVolume ) return kResultFalse;
        id = kParamGain;
        return kResultOk;
    }

private:
    ~TestSineController() = default;
    static double clamp01( double v ) { return v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v ); }
    static void copyName2( Vst::String128 dst, const char *src )
    {
        std::size_t i = 0;
        for( ; src[i] && i < 127; ++i ) dst[i] = (Vst::TChar)(unsigned char)src[i];
        dst[i] = 0;
    }

    int32                  refs_ = 1;
    double                 gain_ = 1.0;
    Vst::IConnectionPoint *peer_ = nullptr;
};

class TestSine final : public Vst::IComponent,
                       public Vst::IAudioProcessor,
                       public Vst::IConnectionPoint {
public:
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
        if( FUnknownPrivate::iidEqual( _iid, Vst::IConnectionPoint::iid ) ) {
            *obj = static_cast<Vst::IConnectionPoint *>( this );
            addRef();
            return kResultOk;
        }
        // NOTE the absence of IEditController: this is a SPLIT plugin, and a
        // host that found a controller here would never take the split path.
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return (uint32)++refs_; }
    uint32 PLUGIN_API release() override
    {
        if( --refs_ <= 0 ) { delete this; return 0; }
        return (uint32)refs_;
    }

    tresult PLUGIN_API initialize( FUnknown * ) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    tresult PLUGIN_API connect( Vst::IConnectionPoint *other ) override
    {
        peer_ = other;
        return kResultOk;
    }
    tresult PLUGIN_API disconnect( Vst::IConnectionPoint * ) override
    {
        peer_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify( Vst::IMessage * ) override { return kResultOk; }

    // --- IComponent ----------------------------------------------------------
    tresult PLUGIN_API getControllerClassId( TUID cid ) override
    {
        std::memcpy( cid, kTestSineCtrlCid, sizeof( TUID ) );
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode( Vst::IoMode ) override { return kNotImplemented; }

    int32 PLUGIN_API getBusCount( Vst::MediaType type, Vst::BusDirection dir ) override
    {
        if( type == Vst::kAudio ) return dir == Vst::kOutput ? 1 : 0;
        if( type == Vst::kEvent ) return dir == Vst::kInput ? 1 : 0;
        return 0;
    }

    tresult PLUGIN_API getBusInfo( Vst::MediaType type, Vst::BusDirection dir,
                                   int32 index, Vst::BusInfo &bus ) override
    {
        if( index != 0 ) return kInvalidArgument;
        if( type == Vst::kAudio && dir == Vst::kOutput ) {
            bus.mediaType    = Vst::kAudio;
            bus.direction    = dir;
            bus.channelCount = channels_;
            bus.busType      = Vst::kMain;
            bus.flags        = Vst::BusInfo::kDefaultActive;
            copyName3( bus.name, "Out" );
            return kResultOk;
        }
        if( type == Vst::kEvent && dir == Vst::kInput ) {
            bus.mediaType    = Vst::kEvent;
            bus.direction    = dir;
            bus.channelCount = 16;
            bus.busType      = Vst::kMain;
            bus.flags        = Vst::BusInfo::kDefaultActive;
            copyName3( bus.name, "Notes In" );
            return kResultOk;
        }
        return kInvalidArgument;
    }

    tresult PLUGIN_API getRoutingInfo( Vst::RoutingInfo &, Vst::RoutingInfo & ) override
    {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus( Vst::MediaType type, Vst::BusDirection dir,
                                    int32 index, TBool state ) override
    {
        if( type == Vst::kEvent && dir == Vst::kInput && index == 0 )
            eventBusActive_ = state != 0;
        return kResultOk;
    }
    tresult PLUGIN_API setActive( TBool state ) override
    {
        active_ = state != 0;
        if( !active_ ) allNotesOff();
        return kResultOk;
    }

    tresult PLUGIN_API setState( IBStream *state ) override
    {
        if( !state ) return kInvalidArgument;
        uint32 magic = 0;
        double gain  = 1.0;
        int32  got   = 0;
        if( state->read( &magic, (int32)sizeof( magic ), &got ) != kResultOk ||
            got != (int32)sizeof( magic ) || magic != kSineStateMagic )
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
        uint32 magic = kSineStateMagic;
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
        (void)inputs;
        if( numIns != 0 || numOuts != 1 || !outputs ) return kResultFalse;
        const int32 ch = countChannels2( outputs[0] );
        if( ch < 1 || ch > kMaxChannels ) return kResultFalse;
        channels_ = ch;
        return kResultOk;
    }
    tresult PLUGIN_API getBusArrangement( Vst::BusDirection dir, int32 index,
                                          Vst::SpeakerArrangement &arr ) override
    {
        if( dir != Vst::kOutput || index != 0 ) return kInvalidArgument;
        arr = channels_ == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize( int32 s ) override
    {
        return s == Vst::kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    uint32 PLUGIN_API getTailSamples() override { return 0; }
    tresult PLUGIN_API setupProcessing( Vst::ProcessSetup &setup ) override
    {
        if( setup.symbolicSampleSize != Vst::kSample32 ) return kResultFalse;
        maxBlock_   = setup.maxSamplesPerBlock;
        sampleRate_ = setup.sampleRate > 0.0 ? setup.sampleRate : 48000.0;
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing( TBool state ) override
    {
        processing_ = state != 0;
        if( !processing_ ) allNotesOff();
        return kResultOk;
    }

    tresult PLUGIN_API process( Vst::ProcessData &data ) override
    {
        if( data.numSamples <= 0 ) return kResultOk;
        if( !data.outputs || data.numOutputs < 1 ) return kResultOk;
        if( data.symbolicSampleSize != Vst::kSample32 ) return kResultFalse;

        Vst::AudioBusBuffers &out = data.outputs[0];

        // THE UNACTIVATED-BUS RULE. With the event bus off we do not look at
        // inputEvents at all, so no voice ever starts and the render is silent.
        // A host that forgets activateBus(kEvent, kInput, 0, true) therefore
        // fails a LEVEL assertion instead of failing nothing (AC3).
        const int32 nEvents = ( eventBusActive_ && data.inputEvents )
                                  ? data.inputEvents->getEventCount() : 0;

        // Walk the block frame-boundary by frame-boundary: apply everything due
        // at `pos`, then render up to the next due offset. Both notes and
        // parameter points therefore take effect at their own sample.
        int32 pos   = 0;
        int32 evIdx = 0;
        while( pos < data.numSamples ) {
            // Everything scheduled at or before `pos`.
            while( evIdx < nEvents ) {
                Vst::Event e{};
                if( data.inputEvents->getEvent( evIdx, e ) != kResultOk ) { ++evIdx; continue; }
                int32 off = e.sampleOffset < 0 ? 0 : e.sampleOffset;
                if( off > pos ) break;
                applyEvent( e );
                ++evIdx;
            }
            applyParamAt( data, pos );

            // The next boundary strictly after `pos`.
            int32 next = data.numSamples;
            if( evIdx < nEvents ) {
                Vst::Event e{};
                if( data.inputEvents->getEvent( evIdx, e ) == kResultOk ) {
                    int32 off = e.sampleOffset < 0 ? 0 : e.sampleOffset;
                    if( off < next ) next = off;
                }
            }
            const int32 nextP = nextParamOffset( data, pos );
            if( nextP >= 0 && nextP < next ) next = nextP;
            if( next <= pos ) next = pos + 1;   // cannot fail to advance

            const int32 to = next > data.numSamples ? data.numSamples : next;
            renderRange( out, pos, to );
            pos = to;
        }

        // Anything the host scheduled past the end still belongs to this block.
        while( evIdx < nEvents ) {
            Vst::Event e{};
            if( data.inputEvents->getEvent( evIdx, e ) == kResultOk )
                applyEvent( e );
            ++evIdx;
        }

        out.silenceFlags = 0;
        return kResultOk;
    }

private:
    struct Voice {
        bool   active   = false;
        int32  noteId   = -1;
        int16  pitch    = -1;
        int16  channel  = 0;
        double velocity = 0.0;
        double phase    = 0.0;
        double inc      = 0.0;
    };

    ~TestSine() = default;

    void allNotesOff()
    {
        for( int32 i = 0; i < kSineVoices; ++i ) voices_[i] = Voice{};
    }

    void applyEvent( const Vst::Event &e )
    {
        if( e.type == Vst::Event::kNoteOnEvent ) {
            for( int32 i = 0; i < kSineVoices; ++i ) {
                if( voices_[i].active ) continue;
                voices_[i].active   = true;
                voices_[i].noteId   = e.noteOn.noteId;
                voices_[i].pitch    = e.noteOn.pitch;
                voices_[i].channel  = e.noteOn.channel;
                voices_[i].velocity = e.noteOn.velocity;
                voices_[i].phase    = 0.0;   // deterministic (AC5)
                voices_[i].inc      = pitchToHz( e.noteOn.pitch ) / sampleRate_;
                return;
            }
        } else if( e.type == Vst::Event::kNoteOffEvent ) {
            // Matched by noteId when the host issued one, else by pitch — the
            // ABI rule made observable: a host that sends a DIFFERENT id on the
            // off leaves the note hanging, which a silence assertion catches.
            for( int32 i = 0; i < kSineVoices; ++i ) {
                if( !voices_[i].active ) continue;
                const bool byId  = e.noteOff.noteId >= 0 && voices_[i].noteId == e.noteOff.noteId;
                const bool byKey = e.noteOff.noteId < 0 && voices_[i].pitch == e.noteOff.pitch;
                if( byId || byKey ) { voices_[i] = Voice{}; return; }
            }
        }
    }

    void renderRange( Vst::AudioBusBuffers &out, int32 from, int32 to )
    {
        if( to <= from ) return;
        const float g = (float)gain_;
        for( int32 s = from; s < to; ++s ) {
            double sum = 0.0;
            for( int32 i = 0; i < kSineVoices; ++i ) {
                Voice &v = voices_[i];
                if( !v.active ) continue;
                sum += v.velocity * std::sin( 2.0 * 3.14159265358979323846 * v.phase );
                v.phase += v.inc;
                if( v.phase >= 1.0 ) v.phase -= std::floor( v.phase );
            }
            const float y = (float)sum * g;
            for( int32 c = 0; c < out.numChannels; ++c )
                if( out.channelBuffers32 && out.channelBuffers32[c] )
                    out.channelBuffers32[c][s] = y;
        }
    }

    int32 nextParamOffset( Vst::ProcessData &data, int32 after )
    {
        if( !data.inputParameterChanges ) return -1;
        int32 best = -1;
        const int32 nq = data.inputParameterChanges->getParameterCount();
        for( int32 q = 0; q < nq; ++q ) {
            Vst::IParamValueQueue *queue = data.inputParameterChanges->getParameterData( q );
            if( !queue || queue->getParameterId() != kParamGain ) continue;
            const int32 nPoints = queue->getPointCount();
            for( int32 pi = 0; pi < nPoints; ++pi ) {
                int32           off = 0;
                Vst::ParamValue v   = 0.0;
                if( queue->getPoint( pi, off, v ) != kResultOk ) continue;
                if( off < 0 ) off = 0;
                if( off > after && ( best < 0 || off < best ) ) best = off;
            }
        }
        return best;
    }

    void applyParamAt( Vst::ProcessData &data, int32 at )
    {
        if( !data.inputParameterChanges ) return;
        const int32 nq = data.inputParameterChanges->getParameterCount();
        for( int32 q = 0; q < nq; ++q ) {
            Vst::IParamValueQueue *queue = data.inputParameterChanges->getParameterData( q );
            if( !queue || queue->getParameterId() != kParamGain ) continue;
            const int32 nPoints = queue->getPointCount();
            for( int32 pi = 0; pi < nPoints; ++pi ) {
                int32           off = 0;
                Vst::ParamValue v   = 0.0;
                if( queue->getPoint( pi, off, v ) != kResultOk ) continue;
                if( off < 0 ) off = 0;
                if( off == at ) gain_ = clamp01( v );
            }
        }
    }

    static double pitchToHz( int16 pitch )
    {
        return 440.0 * std::pow( 2.0, ( (double)pitch - 69.0 ) / 12.0 );
    }
    static double clamp01( double v ) { return v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v ); }
    static int32 countChannels2( Vst::SpeakerArrangement a )
    {
        int32 n = 0;
        for( int i = 0; i < 64; ++i )
            if( a & ( (Vst::SpeakerArrangement)1 << i ) ) ++n;
        return n;
    }
    static void copyName3( Vst::String128 dst, const char *src )
    {
        std::size_t i = 0;
        for( ; src[i] && i < 127; ++i ) dst[i] = (Vst::TChar)(unsigned char)src[i];
        dst[i] = 0;
    }

    int32                  refs_           = 1;
    int32                  channels_       = 2;
    int32                  maxBlock_       = 0;
    double                 sampleRate_     = 48000.0;
    bool                   active_         = false;
    bool                   processing_     = false;
    bool                   eventBusActive_ = false;   // NOT active until told
    double                 gain_           = 1.0;
    Vst::IConnectionPoint *peer_           = nullptr;
    Voice                  voices_[kSineVoices];
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

    // THREE classes since proposal 36 P2: the gain effect, the sine INSTRUMENT
    // and the instrument's separate CONTROLLER. Only the first two carry
    // kVstAudioEffectClass, so the host's scanner still sees exactly two
    // plugins — a controller class is reached through getControllerClassId, not
    // by enumeration.
    int32 PLUGIN_API countClasses() override { return 3; }

    tresult PLUGIN_API getClassInfo( int32 index, PClassInfo *info ) override
    {
        if( !info ) return kInvalidArgument;
        *info = PClassInfo();
        info->cardinality = PClassInfo::kManyInstances;
        switch( index ) {
        case 0:
            std::memcpy( info->cid, kTestGainCid, sizeof( TUID ) );
            std::strncpy( info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1 );
            std::strncpy( info->name, "TW Test VST3 Gain", PClassInfo::kNameSize - 1 );
            return kResultOk;
        case 1:
            std::memcpy( info->cid, kTestSineCid, sizeof( TUID ) );
            std::strncpy( info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1 );
            std::strncpy( info->name, "TW Test VST3 Sine", PClassInfo::kNameSize - 1 );
            return kResultOk;
        case 2:
            std::memcpy( info->cid, kTestSineCtrlCid, sizeof( TUID ) );
            std::strncpy( info->category, kVstComponentControllerClass,
                          PClassInfo::kCategorySize - 1 );
            std::strncpy( info->name, "TW Test VST3 Sine Controller",
                          PClassInfo::kNameSize - 1 );
            return kResultOk;
        default:
            return kInvalidArgument;
        }
    }

    tresult PLUGIN_API getClassInfo2( int32 index, PClassInfo2 *info ) override
    {
        if( !info ) return kInvalidArgument;
        *info = PClassInfo2();
        info->cardinality = PClassInfo::kManyInstances;
        info->classFlags  = 0;
        std::strncpy( info->vendor, "Smaragd", PClassInfo2::kVendorSize - 1 );
        std::strncpy( info->version, "1.0.0", PClassInfo2::kVersionSize - 1 );
        std::strncpy( info->sdkVersion, "VST 3.7", PClassInfo2::kVersionSize - 1 );
        switch( index ) {
        case 0:
            std::memcpy( info->cid, kTestGainCid, sizeof( TUID ) );
            std::strncpy( info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1 );
            std::strncpy( info->name, "TW Test VST3 Gain", PClassInfo::kNameSize - 1 );
            std::strncpy( info->subCategories, "Fx", PClassInfo2::kSubCategoriesSize - 1 );
            return kResultOk;
        case 1:
            std::memcpy( info->cid, kTestSineCid, sizeof( TUID ) );
            std::strncpy( info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1 );
            std::strncpy( info->name, "TW Test VST3 Sine", PClassInfo::kNameSize - 1 );
            // VST3 has no instrument CATEGORY: an instrument is an audio-effect
            // class whose subCategories say "Instrument".
            std::strncpy( info->subCategories, "Instrument|Synth",
                          PClassInfo2::kSubCategoriesSize - 1 );
            return kResultOk;
        case 2:
            std::memcpy( info->cid, kTestSineCtrlCid, sizeof( TUID ) );
            std::strncpy( info->category, kVstComponentControllerClass,
                          PClassInfo::kCategorySize - 1 );
            std::strncpy( info->name, "TW Test VST3 Sine Controller",
                          PClassInfo::kNameSize - 1 );
            return kResultOk;
        default:
            return kInvalidArgument;
        }
    }

    tresult PLUGIN_API createInstance( FIDString cid, FIDString _iid, void **obj ) override
    {
        if( !cid || !_iid || !obj ) return kInvalidArgument;
        *obj = nullptr;

        // FIDString and `const TUID` are both const char* after decay; no cast.
        if( FUnknownPrivate::iidEqual( cid, kTestGainCid ) ) {
            TestGain *p = new( std::nothrow ) TestGain();
            if( !p ) return kOutOfMemory;
            const tresult r = p->queryInterface( _iid, obj );
            p->release();   // the QI above took the reference the caller keeps
            return r;
        }
        if( FUnknownPrivate::iidEqual( cid, kTestSineCid ) ) {
            TestSine *p = new( std::nothrow ) TestSine();
            if( !p ) return kOutOfMemory;
            const tresult r = p->queryInterface( _iid, obj );
            p->release();
            return r;
        }
        if( FUnknownPrivate::iidEqual( cid, kTestSineCtrlCid ) ) {
            TestSineController *p = new( std::nothrow ) TestSineController();
            if( !p ) return kOutOfMemory;
            const tresult r = p->queryInterface( _iid, obj );
            p->release();
            return r;
        }
        return kNoInterface;
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
