// asio_channels — turning a "which inputs do I want" bitmask into the channel
// set an ASIO driver is asked to open, and into the stream width that set
// implies. Proposal 35, Phase 3.
//
// PRIVATE header (devices/src/), SDK-free and platform-free on purpose, so
// `multi_backend_test` can gate the arithmetic on any host. Every decision
// here is one that produces SILENTLY MIS-ROUTED AUDIO when it is wrong rather
// than an error, which is exactly the kind that deserves a unit test.
//
// THE ONE RULE EVERYTHING ELSE FOLLOWS: **bit n means input n**, in the mask,
// in the opened set, and in the interleaved stream the ring carries. That is
// the convention `SObject::recordingChannels_` already uses and that the
// capture bridge applies per WAV sink (`CaptureWavSink::channelMask`).
//
// It is why the stream is NOT COMPACTED. Opening inputs {0, 5} and handing up
// a 2-channel stream would be the memory-efficient thing and would silently
// redefine every mask in the project: bit 5 would come to mean "the second
// channel I happened to open" rather than IN 6, and a take would land on the
// wrong input with nothing to show for it. So the stream is
// `max(opened) + 1` wide and the channels nobody asked for are SILENT. The
// conversion cost still scales with what is actually open — the callback
// converts opened channels only — while the indices stay honest.
//
// GROW-ONLY: `unionMask` never drops a bit. Arming a channel a second time is
// then free, disarming never disturbs a running stream, and the open set
// converges on the union of what a session has used.

#pragma once

#include <cstdint>
#include <vector>

namespace audio {

// The channel indices a mask names, ascending. Bits at or above `available`
// are IGNORED rather than rejected: a project saved on a 16-input interface
// and opened on a 2-input one must still arm what it can instead of failing.
inline std::vector<int> asioChannelsFromMask(std::uint64_t mask, int available)
{
    std::vector<int> out;
    if (available <= 0) return out;
    const int n = available < 64 ? available : 64;
    for (int i = 0; i < n; ++i)
        if (mask & (std::uint64_t(1) << i)) out.push_back(i);
    return out;
}

// The width of the interleaved stream a set of opened channels implies:
// max index + 1, so bit n stays input n. Empty set -> 0.
inline int asioStreamWidthFor(std::uint64_t mask, int available)
{
    const std::vector<int> ch = asioChannelsFromMask(mask, available);
    return ch.empty() ? 0 : ch.back() + 1;
}

// Grow-only union. Bits above `available` are dropped here too, so the stored
// set can never name a channel the driver does not have.
inline std::uint64_t asioGrowMask(std::uint64_t current, std::uint64_t wanted, int available)
{
    if (available <= 0) return current;
    const int n = available < 64 ? available : 64;
    const std::uint64_t valid =
        (n >= 64) ? ~std::uint64_t(0) : ((std::uint64_t(1) << n) - 1);
    return current | (wanted & valid);
}

// Is everything `wanted` asks for already open? The question a request has to
// answer before deciding whether it may disturb a running stream.
inline bool asioMaskSatisfied(std::uint64_t open, std::uint64_t wanted, int available)
{
    if (available <= 0) return true;  // nothing to open; nothing to wait for
    const int n = available < 64 ? available : 64;
    const std::uint64_t valid =
        (n >= 64) ? ~std::uint64_t(0) : ((std::uint64_t(1) << n) - 1);
    return ((wanted & valid) & ~open) == 0;
}

// What a device opens when nobody has asked for anything: INPUT 0 ALONE.
// It matches `SObject::DEFAULT_RECORDING_CHANNELS` (bit 0, the first input),
// so a session that never calls requestChannels still records the channel the
// app would have selected by default — and opens one channel rather than a
// pro interface's sixteen.
inline constexpr std::uint64_t kAsioDefaultInputMask = 1;

}  // namespace audio
