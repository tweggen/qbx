// asio_convert — interleaved float <-> one ASIO channel's native sample type.
//
// PRIVATE header (devices/src/), and deliberately free of the ASIO SDK: the
// type is passed as a plain enum of our own, so `multi_backend_test` builds
// and runs these on macOS and Linux. Proposal 35, Phase 2.
//
// WHY THIS EXISTS RATHER THAN twConvertFrames. Two reasons, both structural:
//
//   1. ASIO is DE-INTERLEAVED. Every channel has its own driver-owned buffer,
//      and may have its own sample type. `twConvertFrames` converts one
//      interleaved block to another interleaved block, which is the wrong
//      shape at both ends — the read is strided and the write is contiguous.
//   2. `twSampleType` has no Int24, and ASIO's Int24 is PACKED (three bytes
//      per sample, not a 32-bit container). Adding it to the engine's format
//      enum to serve one device backend would put a wire format nothing else
//      can produce into the core.
//
// Everything here is per-channel and takes the interleaved stride explicitly,
// so the caller loops over channels and never allocates. Nothing here logs,
// allocates or locks: it runs inside `bufferSwitch`, on a driver-owned thread,
// under RT rules.
//
// ENDIANNESS: little-endian only, by design. ASIO's MSB types and the DSD
// types are refused at open with a clear error rather than half-supported —
// see `asioTypeSupported`. Windows on x64 is the only platform this compiles
// for in production, but the conversions themselves are written without a
// reinterpret_cast on multi-byte quantities so the unit test is meaningful on
// any host.

#pragma once

#include <cstdint>
#include <cstring>

namespace audio {

// Mirrors the ASIOSampleType values we support. Kept as our own enum so this
// header stays SDK-free; `asio_device.cc` maps ASIOSampleType onto it once, at
// open time, and refuses anything not listed.
enum class AsioType {
    Unsupported = 0,
    Int16LSB,
    Int24LSB,   // PACKED: 3 bytes per sample
    Int32LSB,
    Float32LSB,
    Float64LSB,
};

// --- clamping ---------------------------------------------------------------
//
// A render callback may hand back a sample outside [-1, 1]: nothing upstream
// clips, and proposal 36 B4's golden re-freeze exists precisely because a
// saturating render was once the reference. Wrapping an out-of-range value
// into an integer type is the loudest possible failure (a full-scale positive
// peak becomes a full-scale negative one), so every integer conversion clamps.
// The float types deliberately do NOT clamp: the device gets exactly what the
// engine produced, which is what a float driver path is for.
inline float asioClamp1(float v)
{
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

// --- float -> native (the OUTPUT half) --------------------------------------
//
// Reads `frames` samples of channel `ch` out of an interleaved float block of
// `stride` channels, and writes them contiguously into the driver's buffer.
inline void asioFromFloat(void *dst, AsioType type, const float *src,
                          std::size_t frames, std::size_t stride, std::size_t ch)
{
    if (!dst || !src || stride == 0) return;

    switch (type) {
    case AsioType::Int16LSB: {
        auto *d = static_cast<std::uint8_t *>(dst);
        for (std::size_t i = 0; i < frames; ++i) {
            const float f = asioClamp1(src[i * stride + ch]);
            // 32767, not 32768: the positive and negative full scales are
            // asymmetric in two's complement, and scaling by 32768 makes +1.0
            // wrap to -32768.
            const std::int32_t v = (std::int32_t) (f * 32767.0f);
            d[i * 2 + 0] = (std::uint8_t) (v & 0xFF);
            d[i * 2 + 1] = (std::uint8_t) ((v >> 8) & 0xFF);
        }
        return;
    }
    case AsioType::Int24LSB: {
        auto *d = static_cast<std::uint8_t *>(dst);
        for (std::size_t i = 0; i < frames; ++i) {
            const float f = asioClamp1(src[i * stride + ch]);
            const std::int32_t v = (std::int32_t) (f * 8388607.0f);
            d[i * 3 + 0] = (std::uint8_t) (v & 0xFF);
            d[i * 3 + 1] = (std::uint8_t) ((v >> 8) & 0xFF);
            d[i * 3 + 2] = (std::uint8_t) ((v >> 16) & 0xFF);
        }
        return;
    }
    case AsioType::Int32LSB: {
        auto *d = static_cast<std::uint8_t *>(dst);
        for (std::size_t i = 0; i < frames; ++i) {
            const float f = asioClamp1(src[i * stride + ch]);
            // Via double: 2147483647.0f is not representable as a float, so a
            // float multiply would round the scale itself before the sample.
            const std::int32_t v = (std::int32_t) ((double) f * 2147483647.0);
            d[i * 4 + 0] = (std::uint8_t) (v & 0xFF);
            d[i * 4 + 1] = (std::uint8_t) ((v >> 8) & 0xFF);
            d[i * 4 + 2] = (std::uint8_t) ((v >> 16) & 0xFF);
            d[i * 4 + 3] = (std::uint8_t) ((v >> 24) & 0xFF);
        }
        return;
    }
    case AsioType::Float32LSB: {
        auto *d = static_cast<std::uint8_t *>(dst);
        for (std::size_t i = 0; i < frames; ++i) {
            const float f = src[i * stride + ch];
            std::memcpy(d + i * 4, &f, 4);
        }
        return;
    }
    case AsioType::Float64LSB: {
        auto *d = static_cast<std::uint8_t *>(dst);
        for (std::size_t i = 0; i < frames; ++i) {
            const double f = (double) src[i * stride + ch];
            std::memcpy(d + i * 8, &f, 8);
        }
        return;
    }
    case AsioType::Unsupported:
    default:
        return;  // open() refuses these; a stray call writes nothing.
    }
}

// --- native -> float (the INPUT half, Phase 3) ------------------------------
//
// Written here with its twin because the two halves must agree on the scale
// factors, and a scale that disagrees is inaudible on either path alone.
inline void asioToFloat(float *dst, std::size_t stride, std::size_t ch,
                        const void *src, AsioType type, std::size_t frames)
{
    if (!dst || !src || stride == 0) return;

    switch (type) {
    case AsioType::Int16LSB: {
        auto *s = static_cast<const std::uint8_t *>(src);
        for (std::size_t i = 0; i < frames; ++i) {
            const std::int16_t v = (std::int16_t) ((std::uint16_t) s[i * 2 + 0] |
                                                   ((std::uint16_t) s[i * 2 + 1] << 8));
            dst[i * stride + ch] = (float) v / 32768.0f;
        }
        return;
    }
    case AsioType::Int24LSB: {
        auto *s = static_cast<const std::uint8_t *>(src);
        for (std::size_t i = 0; i < frames; ++i) {
            std::int32_t v = (std::int32_t) ((std::uint32_t) s[i * 3 + 0] |
                                             ((std::uint32_t) s[i * 3 + 1] << 8) |
                                             ((std::uint32_t) s[i * 3 + 2] << 16));
            if (v & 0x800000) v |= ~0xFFFFFF;  // sign-extend 24 -> 32
            dst[i * stride + ch] = (float) v / 8388608.0f;
        }
        return;
    }
    case AsioType::Int32LSB: {
        auto *s = static_cast<const std::uint8_t *>(src);
        for (std::size_t i = 0; i < frames; ++i) {
            const std::int32_t v = (std::int32_t) ((std::uint32_t) s[i * 4 + 0] |
                                                   ((std::uint32_t) s[i * 4 + 1] << 8) |
                                                   ((std::uint32_t) s[i * 4 + 2] << 16) |
                                                   ((std::uint32_t) s[i * 4 + 3] << 24));
            dst[i * stride + ch] = (float) ((double) v / 2147483648.0);
        }
        return;
    }
    case AsioType::Float32LSB: {
        auto *s = static_cast<const std::uint8_t *>(src);
        for (std::size_t i = 0; i < frames; ++i) {
            float f = 0.0f;
            std::memcpy(&f, s + i * 4, 4);
            dst[i * stride + ch] = f;
        }
        return;
    }
    case AsioType::Float64LSB: {
        auto *s = static_cast<const std::uint8_t *>(src);
        for (std::size_t i = 0; i < frames; ++i) {
            double f = 0.0;
            std::memcpy(&f, s + i * 8, 8);
            dst[i * stride + ch] = (float) f;
        }
        return;
    }
    case AsioType::Unsupported:
    default:
        for (std::size_t i = 0; i < frames; ++i) dst[i * stride + ch] = 0.0f;
        return;
    }
}

inline bool asioTypeSupported(AsioType t) { return t != AsioType::Unsupported; }

inline const char *asioTypeName(AsioType t)
{
    switch (t) {
    case AsioType::Int16LSB:   return "Int16LSB";
    case AsioType::Int24LSB:   return "Int24LSB";
    case AsioType::Int32LSB:   return "Int32LSB";
    case AsioType::Float32LSB: return "Float32LSB";
    case AsioType::Float64LSB: return "Float64LSB";
    default:                   return "unsupported";
    }
}

// Bytes one sample of `t` occupies in a driver buffer. Int24 is 3, which is
// the whole reason this function exists rather than a sizeof().
inline std::size_t asioTypeBytes(AsioType t)
{
    switch (t) {
    case AsioType::Int16LSB:   return 2;
    case AsioType::Int24LSB:   return 3;
    case AsioType::Int32LSB:   return 4;
    case AsioType::Float32LSB: return 4;
    case AsioType::Float64LSB: return 8;
    default:                   return 0;
    }
}

}  // namespace audio
