// VST3 interface IID definitions (proposal 08 M6).
//
// WHY THIS FILE EXISTS. `vst3_pluginterfaces` — the submodule we vendor — ships
// base/coreiids.cpp, and that file defines the IIDs of the BASE interfaces only:
// FUnknown, IBStream, ISizeableStream, IPluginBase, IPluginFactory{,2,3},
// ICloneable, IDependent, IUpdateHandler. Every IID of the VST module itself
// lives in public.sdk/source/vst/vstinitiids.cpp in the FULL SDK, which the
// pluginterfaces-only submodule does not contain.
//
// So the host must define them, exactly once per program. Get this wrong and
// nothing complains at compile time and nothing complains at link time either
// until the first `Vst::IComponent::iid` reference — DECLARE_CLASS_IID only
// declares `static const FUID iid`, so a missing definition is an undefined
// symbol at link, or worse a silently different one if two translation units
// each roll their own.
//
// This corrects 08_PLUGIN_HOSTING_EXECUTION.md M6, which listed the SDK sources
// to compile as base/{funknown,coreiids,ustring,conststringtable}.cpp and
// stopped there. That list is necessary and not sufficient; this file is the
// remainder. It was found by building tools/vst3_probe.cc, which carried the
// same block inline as a spike.
//
// Keep this list to interfaces the backend ACTUALLY touches. An IID defined for
// an interface nobody uses is dead weight that still has to be kept in step with
// the header it mirrors.

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"

namespace Steinberg {

// --- plugin-side interfaces the host queries for ----------------------------
DEF_CLASS_IID( Vst::IComponent )
DEF_CLASS_IID( Vst::IAudioProcessor )
DEF_CLASS_IID( Vst::IEditController )
DEF_CLASS_IID( Vst::IConnectionPoint )

// --- host-side interfaces the PLUGIN queries us for -------------------------
// A plugin that cannot find IHostApplication on the context we hand to
// initialize() will often still load, but in a reduced mode; one that cannot
// find IComponentHandler cannot report parameter edits at all.
DEF_CLASS_IID( Vst::IHostApplication )
DEF_CLASS_IID( Vst::IComponentHandler )
DEF_CLASS_IID( Vst::IPlugInterfaceSupport )

// --- objects passed across the boundary in both directions ------------------
// IMessage/IAttributeList are how a split component/controller pair talks to
// itself THROUGH the host; the host has to be able to manufacture them
// (IHostApplication::createInstance) and to route them (IConnectionPoint::notify).
DEF_CLASS_IID( Vst::IMessage )
DEF_CLASS_IID( Vst::IAttributeList )
DEF_CLASS_IID( Vst::IParameterChanges )
DEF_CLASS_IID( Vst::IParamValueQueue )

// --- events (proposal 37 P2) ------------------------------------------------
// IEventList travels in BOTH directions (ProcessData::inputEvents is ours,
// outputEvents is ours too and the plugin fills it). IMidiMapping and
// INoteExpressionController are queried on the plugin's CONTROLLER — a CC has
// no VST3 event type at all, so the CC->parameter map IS the only route, and
// note expressions are declared by the controller rather than the processor.
DEF_CLASS_IID( Vst::IEventList )
DEF_CLASS_IID( Vst::IMidiMapping )
DEF_CLASS_IID( Vst::INoteExpressionController )

}  // namespace Steinberg
