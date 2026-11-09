// coreaudio_rate — should CoreAudioBackend switch the device's nominal sample
// rate to the project's, and if not, why not (QBX-293).
//
// PRIVATE header (devices/src/), SDK-free and pure, so `multi_backend_test`
// can assert the decision on any platform without a Mac. The CoreAudio calls
// around it (reading the offered ranges, setting the property, waiting for the
// change to take) live in coreaudio_backend.cc and have no headless gate.
//
// WHY SWITCH (author's decision, 2026-11-09). The live lane requires device
// rate == project rate (playback CONTRACT inv. 18): its entries are stamped in
// project frames and have no resampler. A Mac's output device is commonly left
// at 44.1 kHz, so a 48 kHz project played resampled and never monitored. Logic,
// REAPER and qbx's own ASIO path set the device to the project rate instead;
// so does this. When the switch is impossible the backend falls back to what
// it did before: playback resampled, the live lane refused with its message.
//
// A device lists the rates it OFFERS as ranges
// (kAudioDevicePropertyAvailableNominalSampleRates); a discrete rate is a
// range whose minimum equals its maximum.

#pragma once

#include <cstdint>
#include <vector>

namespace audio {

struct CoreAudioRateRange {
    double minimum = 0.0;
    double maximum = 0.0;
};

enum class CoreAudioRatePlan {
    NoPreference,   // the caller asked for no particular rate (0)
    Keep,           // the device already runs at the wanted rate
    Switch,         // set the nominal rate to the wanted one
    NotOffered,     // the device does not offer the wanted rate
    NotSettable,    // the device offers it but will not let it be set
};

inline bool coreAudioRateOffered(std::uint32_t rate,
                                 const std::vector<CoreAudioRateRange> &offered)
{
    // Half a hertz of slack: the ranges are Float64 and a device reporting
    // 44099.99 for 44100 must still count.
    const double r = static_cast<double>(rate);
    for (const CoreAudioRateRange &range : offered) {
        if (r >= range.minimum - 0.5 && r <= range.maximum + 0.5) return true;
    }
    return false;
}

inline CoreAudioRatePlan planCoreAudioRate(std::uint32_t current, std::uint32_t wanted,
                                           const std::vector<CoreAudioRateRange> &offered,
                                           bool settable)
{
    if (wanted == 0) return CoreAudioRatePlan::NoPreference;
    if (current == wanted) return CoreAudioRatePlan::Keep;
    if (!coreAudioRateOffered(wanted, offered)) return CoreAudioRatePlan::NotOffered;
    if (!settable) return CoreAudioRatePlan::NotSettable;
    return CoreAudioRatePlan::Switch;
}

inline const char *coreAudioRatePlanName(CoreAudioRatePlan p)
{
    switch (p) {
    case CoreAudioRatePlan::NoPreference: return "no preference";
    case CoreAudioRatePlan::Keep:         return "already at the project rate";
    case CoreAudioRatePlan::Switch:       return "switch";
    case CoreAudioRatePlan::NotOffered:   return "the device does not offer the project rate";
    case CoreAudioRatePlan::NotSettable:  return "the device's rate is not settable";
    }
    return "?";
}

}  // namespace audio
