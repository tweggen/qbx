#ifndef _TWSAMPLERHOST_H_
#define _TWSAMPLERHOST_H_

// twsamplerhost.h — the sampler's extension interface (proposal 56 §4.2,
// nassau-digital DESIGN §9.1).
//
// REACHED BY CAPABILITY: `dynamic_cast<audio::twSamplerHost *>( twPlugin * )`.
// twPlugin has no generic extension query, and adding one for a single
// consumer would widen the narrow interface for nothing; the cast is the
// honest spelling of "capability-queried" until a second extension exists.
// A plugin that is not the sampler (including a Missing slot's placeholder)
// answers null.
//
// EVERY CALL GOES THROUGH twPluginSlotProcessor::applyToInstances() (proposal
// 56 invariant 3): that holds the processor's mutex, the one render() holds for
// its whole call, so an install lands between two process() calls and
// process() reads the snapshots WITHOUT a lock of its own (R3). Never call a
// method here through the bare twPluginSlotProcessor::plugin() pointer. The
// one exception is stats(), which reads atomics and is safe from anywhere.

#include "tw/sampler/twsamplertypes.h"

#include <cstdint>
#include <memory>

namespace audio {

// What process() itself last ran on: written by process() (relaxed atomics, no
// lock, no allocation), read by tests and, later, by the editor. It is the
// evidence that an install was READ ON THE AUDIO PATH, not merely stored.
struct twSamplerHostStats {
    std::uint64_t processCalls    = 0;  // process() calls since construction
    std::uint64_t programSerial   = 0;  // twSamplerProgram::serial() it ran on; 0 = none
    std::uint32_t routedMask      = 0;  // the routed-outputs mask it ran with
    std::uint32_t samplesInstalled = 0; // layers holding a sample, as it saw them
    int           activeVoices    = 0;  // voices sounding after its last call
    // The serial of the program most recently INSTALLED by setProgram() (0 =
    // none), whether or not process() has run since (proposal 56 Q1). It is
    // what the model's "every edit reaches every instance, and a rebuilt one is
    // born current" is asserted on without a render: programSerial above says
    // what process() READ, this says what the instance HOLDS.
    std::uint64_t installedSerial = 0;
};

class twSamplerHost {
public:
    // Install a program snapshot (null clears it: the instrument is silent).
    virtual void setProgram( std::shared_ptr<const twSamplerProgram> program ) = 0;
    // Install the buffer layer `layerId` plays (null removes it). A voice keeps
    // the buffer that was current at its note-on until it ends.
    virtual void setSample( std::uint32_t layerId,
                            std::shared_ptr<const twSamplerSample> sample ) = 0;
    // Which aux outputs are routed: bit k = aux k+1 (DESIGN §3.1). Until Q8 the
    // instrument reports ONE output bus (proposal 56 B7), so an aux zone plays
    // on main whatever this says; it is stored and read by process() now so the
    // seam is there when the buses are.
    virtual void setRoutedOutputs( std::uint32_t mask ) = 0;
    // Free replaced buffers no voice holds any more. Host thread, NEVER the
    // audio thread; process() never drops the last reference to a buffer.
    // Returns how many were freed.
    virtual int collectRetired() = 0;

    virtual twSamplerHostStats stats() const = 0;

protected:
    ~twSamplerHost() = default;
};

}  // namespace audio

#endif
