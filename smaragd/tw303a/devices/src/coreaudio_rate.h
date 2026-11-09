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

#include "tw/devices/coreaudio_backend.h"

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

// GIVING THE RATE BACK (QBX-296, the author's question from QBX-293). The
// backend remembers the rate a device had before qbx FIRST switched it, and
// sets it back when qbx lets go of that device for good: when the backend is
// destroyed (qbx quits), when it opens another device, or when an open asks
// for no rate (a project that keeps the device's rate, or the startup probe).
// NOT on every close: twSpeaker closes the device at every Stop, and a switch
// costs up to a second of waiting for the HAL at the next Play.
//
// A second switch on the same device keeps the FIRST original, so 44.1k ->
// 48k -> 96k gives back 44.1k. The rate is restored only if the device is
// still at the one qbx set: a rate somebody else chose since then is theirs.
// CoreAudioRateMemory itself is declared in tw/devices/coreaudio_backend.h,
// because the backend holds one by value.

inline void coreAudioRememberSwitch(CoreAudioRateMemory &m, std::uint32_t device,
                                    std::uint32_t before, std::uint32_t wanted)
{
    if (m.device != device) {
        m.device   = device;
        m.original = before;
    }
    m.setTo = wanted;
}

// Must the remembered device be given its rate back before opening `device`
// at `wanted`? Yes when it is another device, or when no rate is wanted.
inline bool coreAudioMustRestoreBeforeOpen(const CoreAudioRateMemory &m,
                                           std::uint32_t device, std::uint32_t wanted)
{
    if (m.device == 0) return false;
    return m.device != device || wanted == 0;
}

// The rate to set the remembered device back to, given the rate it has NOW;
// 0 means leave it alone.
inline std::uint32_t coreAudioRestoreRate(const CoreAudioRateMemory &m,
                                          std::uint32_t current)
{
    if (m.device == 0 || m.original == 0) return 0;
    if (current != m.setTo) return 0;       // somebody else chose since
    if (current == m.original) return 0;    // already back
    return m.original;
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
