#ifndef _TWSAMPLERREGISTRY_H_
#define _TWSAMPLERREGISTRY_H_

// twsamplerregistry.h — how NassauDigital reaches the plugin registry
// (proposal 56 B2, invariant 2).
//
// tw_plugins cannot name tw_sampler (tw_sampler depends on it), so the
// registry is HANDED the instrument by an explicit call: the composition root
// (SApplication::initPluginRegistry) and every test that wants the instrument
// call registerNassauDigital() themselves. Never a static initializer.
// Idempotent: a second call replaces the first registration.

#include "tw/plugins/twplugindescriptor.h"

namespace audio {

// The instrument's identity in a project file: format "tw", this uid.
extern const char *const kNassauDigitalUid;   // "tw.native.nassaudigital"

// The descriptor the registry lists: a stereo generator, notes in, one bus.
twPluginDescriptor nassauDigitalDescriptor();

void registerNassauDigital( twPluginRegistry &registry );

}  // namespace audio

#endif
