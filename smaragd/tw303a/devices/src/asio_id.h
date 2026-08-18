// asio_id — device-id namespacing for the Windows multi-backend dispatcher.
//
// PRIVATE header (devices/src/), and deliberately FREE of both the ASIO SDK
// and <windows.h>: it is pure string handling, so `multi_backend_test` builds
// and runs it on macOS and Linux too. Proposal 35, Phase 2.
//
// WASAPI endpoints and ASIO drivers appear in ONE device list, so an id has to
// say which world it belongs to:
//
//     wasapi:{0.0.0.00000000}.{guid}      a WASAPI endpoint id
//     asio:{FA12DE15-...}                 an ASIO driver CLSID
//     <anything else>                     WASAPI (see below)
//
// THE FALLBACK IS LOAD-BEARING, not tidiness. Every `audio/deviceId` written
// to smaragd.ini before this existed is a bare WASAPI endpoint id, and so is
// the literal "default"; routing an unprefixed id to WASAPI is what makes a
// user's persisted device selection keep working across the upgrade. The
// per-device settings keys (`audio/outputLatency/<id>`,
// `audio/recordingOffsetMs/<name>`) are unaffected either way, because the key
// is the FULL string as stored, prefix and all.

#pragma once

#include <string>

namespace audio {

enum class DeviceIdKind {
    Wasapi,   // a WASAPI endpoint id, or a bare/legacy id
    Asio,     // an ASIO driver, `native` being its CLSID string
    Default,  // "" or "default": each backend's own idea of the default
};

struct ParsedDeviceId {
    DeviceIdKind kind = DeviceIdKind::Default;
    // The id to hand the underlying backend, with the prefix removed. For
    // Default this is "default", so a caller can always pass it straight on
    // without a second branch.
    std::string native;
};

// Pure, total, and never fails: an unrecognised spelling is a WASAPI id, per
// the fallback rule above. Case-INSENSITIVE on the prefix only — an endpoint
// GUID and a CLSID are compared elsewhere by the APIs that own them, and this
// function must not normalise a payload it does not own.
inline ParsedDeviceId parseDeviceId(const std::string &id)
{
    ParsedDeviceId out;

    auto startsWithNoCase = [&id](const char *pfx) {
        std::size_t n = 0;
        while (pfx[n]) ++n;
        if (id.size() < n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            char a = id[i];
            char b = pfx[i];
            if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    };

    if (id.empty() || id == "default") {
        out.kind   = DeviceIdKind::Default;
        out.native = "default";
        return out;
    }
    if (startsWithNoCase("asio:")) {
        out.kind   = DeviceIdKind::Asio;
        out.native = id.substr(5);
        // "asio:" with nothing after it means "the default ASIO driver", which
        // is the first registered one. Spelling that as an empty native id
        // keeps the caller's branch count at one.
        return out;
    }
    if (startsWithNoCase("wasapi:")) {
        out.kind   = DeviceIdKind::Wasapi;
        out.native = id.substr(7);
        if (out.native.empty()) out.native = "default";
        return out;
    }

    out.kind   = DeviceIdKind::Wasapi;
    out.native = id;
    return out;
}

// The inverse, used when a backend's own enumeration is merged into the one
// list. Never applied to "default": the unprefixed spelling is the one the
// picker and the settings file already use for "let the system decide", and
// prefixing it would make an existing selection unresolvable.
inline std::string makeDeviceId(DeviceIdKind kind, const std::string &native)
{
    if (kind == DeviceIdKind::Default || native.empty() || native == "default")
        return "default";
    return (kind == DeviceIdKind::Asio ? "asio:" : "wasapi:") + native;
}

}  // namespace audio
