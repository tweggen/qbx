#ifndef _AUDIO_PLAYBACK_CONTEXT_H_
#define _AUDIO_PLAYBACK_CONTEXT_H_

#include <cstdint>

class twComponent;

namespace audio {

/**
 * The application-side services twSpeaker needs to run playback, expressed
 * as an engine-owned interface so the engine never includes app headers
 * (modularization proposal 14, Phase 0). The application implements this
 * (SApplication) and hands it to the speaker once at startup; the
 * implementation must outlive the speaker.
 *
 * Threading:
 *  - rootComponent() / locatorPosition() are called on the UI thread from
 *    twSpeaker::startOutput().
 *  - publishPosition() is called on the AUDIO callback thread:
 *    implementations must be lock-free (atomic loads and stores only) and
 *    must not touch Qt. It is the ONE playhead authority (proposal 21 L3b,
 *    design D7) — the `locatorHeldElsewhere()` escape a recording used to
 *    take is retired, because a recording is an ordinary transport run.
 */
struct PlaybackContext {
    virtual ~PlaybackContext() = default;

    // Root of the component graph to play, or null when no project is open.
    virtual std::shared_ptr<twComponent> rootComponent() = 0;

    // Absolute project position (frames) playback should start from.
    virtual std::uint64_t locatorPosition() = 0;

    // True while another authority (e.g. an active recording) owns the
    // playhead; the speaker then refrains from publishing positions.

    // Publish the current playback position (frames). Realtime-safe required.
    virtual void publishPosition(std::uint64_t absPos) = 0;
};

}  // namespace audio

#endif
