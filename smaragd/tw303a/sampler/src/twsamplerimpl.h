#ifndef _TWSAMPLERIMPL_H_
#define _TWSAMPLERIMPL_H_

// The core-facing half of twSamplerProgram / twSamplerSample. PRIVATE to
// tw_sampler: this is the one header that includes the NassauDigital core, and
// it lives in sampler/src/ so no other module can reach it (proposal 56
// invariant 1, the CLAP backend's discipline, plugins/CONTRACT.md inv. 4).

#include "tw/sampler/twsamplertypes.h"

#include "nd_program.h"
#include "nd_sample.h"

#include <cstdint>
#include <memory>

namespace audio {

struct twSamplerProgram::Impl {
    std::shared_ptr<const nd::Program> program;
};

// The next twSamplerProgram::serial(), shared by every way a snapshot is made
// (fromXml, twSamplerProgramModel::snapshot): unique within the process.
std::uint64_t nextSamplerProgramSerial();

struct twSamplerSample::Impl {
    std::shared_ptr<const nd::Sample> sample;
};

}  // namespace audio

#endif
