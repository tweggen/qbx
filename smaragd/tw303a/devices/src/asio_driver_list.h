// asio_driver_list — enumerate installed ASIO drivers (Windows).
//
// PRIVATE header (devices/src/, like wasapi_input.h): only the ASIO backend
// and the asio_probe tool see it. Deliberately SDK-free — an ASIO driver
// installation is three registry values under HKLM\SOFTWARE\ASIO\<name>, so
// enumeration needs advapi32 and nothing from Steinberg. This is what lets us
// skip compiling the SDK's host/pc/asiolist.cpp (MSVC-isms) on MinGW
// entirely; see proposal 35.
//
// The scan is control-plane code: it touches the registry only, never loads
// a driver. Loading (CoCreateInstance) stays with the caller, because a scan
// that instantiates every installed driver would run ASIO4ALL-style splash
// panels and license nags just to fill a combo box.

#pragma once

#include <string>
#include <vector>

namespace audio {

struct AsioDriverEntry {
    std::string name;         // the subkey name — the user-facing driver name
    std::string clsid;        // "{...}" string form, validated by CLSIDFromString
    std::string description;  // the Description value, or name if absent
};

// All validly-registered ASIO drivers (64-bit registry view; entries whose
// CLSID value is missing or unparsable are skipped, not reported). Order is
// the registry's enumeration order. Returns empty on a machine with no
// drivers — not an error.
std::vector<AsioDriverEntry> scanAsioDrivers();

}  // namespace audio
