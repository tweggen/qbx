#include "tw/devices/midi_input.h"
#include "tw/devices/midi_output.h"

#include "tw/devices/capture_midi.h"
#include "tw/devices/keyboard_midi.h"
#include "tw/devices/null_midi.h"

#include "tw/core/twsyslog.h"

#include <cctype>
#include <cstdlib>
#include <string>

#if defined(QBX_WIN_MIDI)
#  include "winmm_midi.h"
#endif
#if defined(QBX_MAC_COREMIDI)
#  include "coremidi_midi.h"
#endif
#if defined(QBX_LINUX_ALSASEQ)
#  include "alsa_seq_midi.h"
#endif

namespace audio {

namespace {

std::string lowered(const char *s)
{
    std::string out;
    for (const char *p = s; p && *p; ++p)
        out.push_back((char) std::tolower((unsigned char) *p));
    return out;
}

// SMARAGD_MIDI_BACKEND, read at every call. Unlike SMARAGD_AUDIO_BACKEND (which
// is read once inside twSpeaker's constructor because there is no call site
// between SApplication and the backend), a MidiOutput is minted from the app's
// MIDI-out pump, where the caller CAN pass a name — hence the two-argument
// overload. The environment variable stays the headless default so a
// --test-case run needs no code path of its own.
std::string wantedBackend()
{
    if (const char *env = std::getenv("SMARAGD_MIDI_BACKEND")) {
        const std::string want = lowered(env);
        if (!want.empty()) return want;
    }
    return "default";
}

void warnUnavailable(const std::string &want)
{
    syslog(LOG_WARNING,
           "midi: backend '%s' is not available in this build; using the platform default",
           want.c_str());
}

void warnUnknown(const std::string &want)
{
    syslog(LOG_WARNING,
           "midi: unknown SMARAGD_MIDI_BACKEND='%s'"
           " (expected winmm|coremidi|alsaseq|capture|null|default);"
           " using the platform backend",
           want.c_str());
}

}  // namespace

std::unique_ptr<MidiOutput> createMidiOutput(const std::string &backend)
{
    const std::string want = backend.empty() ? std::string("default") : lowered(backend.c_str());

    if (want == "capture") return std::unique_ptr<MidiOutput>(new CaptureMidiOutput());
    if (want == "null")    return std::unique_ptr<MidiOutput>(new NullMidiOutput());

    if (want == "winmm") {
#if defined(QBX_WIN_MIDI)
        return std::unique_ptr<MidiOutput>(new WinMMMidiOutput());
#else
        warnUnavailable(want);
#endif
    } else if (want == "coremidi") {
#if defined(QBX_MAC_COREMIDI)
        return std::unique_ptr<MidiOutput>(new CoreMidiOutput());
#else
        warnUnavailable(want);
#endif
    } else if (want == "alsaseq") {
#if defined(QBX_LINUX_ALSASEQ)
        return std::unique_ptr<MidiOutput>(new AlsaSeqMidiOutput());
#else
        warnUnavailable(want);
#endif
    } else if (want != "default") {
        warnUnknown(want);
    }

#if defined(QBX_WIN_MIDI)
    return std::unique_ptr<MidiOutput>(new WinMMMidiOutput());
#elif defined(QBX_MAC_COREMIDI)
    return std::unique_ptr<MidiOutput>(new CoreMidiOutput());
#elif defined(QBX_LINUX_ALSASEQ)
    return std::unique_ptr<MidiOutput>(new AlsaSeqMidiOutput());
#else
    return std::unique_ptr<MidiOutput>(new NullMidiOutput());
#endif
}

std::unique_ptr<MidiOutput> createMidiOutput()
{
    return createMidiOutput(wantedBackend());
}

std::unique_ptr<MidiInput> createMidiInput(const std::string &backend)
{
    const std::string want = backend.empty() ? std::string("default") : lowered(backend.c_str());

    if (want == "capture") return std::unique_ptr<MidiInput>(new CaptureMidiInput());
    if (want == "null")    return std::unique_ptr<MidiInput>(new NullMidiInput());
    // THE COMPUTER KEYBOARD (proposal 21 L2, design D9). Named EXPLICITLY and
    // never reachable through SMARAGD_MIDI_BACKEND: that variable chooses the
    // system MIDI implementation, and the computer keyboard is present on every
    // machine whatever that choice is - so it must not be able to replace the
    // hardware backend, nor be replaced by it.
    if (want == "keyboard") return std::unique_ptr<MidiInput>(new KeyboardMidiInput());

    if (want == "winmm") {
#if defined(QBX_WIN_MIDI)
        return std::unique_ptr<MidiInput>(new WinMMMidiInput());
#else
        warnUnavailable(want);
#endif
    } else if (want == "coremidi") {
#if defined(QBX_MAC_COREMIDI)
        return std::unique_ptr<MidiInput>(new CoreMidiInput());
#else
        warnUnavailable(want);
#endif
    } else if (want == "alsaseq") {
#if defined(QBX_LINUX_ALSASEQ)
        return std::unique_ptr<MidiInput>(new AlsaSeqMidiInput());
#else
        warnUnavailable(want);
#endif
    } else if (want != "default") {
        warnUnknown(want);
    }

#if defined(QBX_WIN_MIDI)
    return std::unique_ptr<MidiInput>(new WinMMMidiInput());
#elif defined(QBX_MAC_COREMIDI)
    return std::unique_ptr<MidiInput>(new CoreMidiInput());
#elif defined(QBX_LINUX_ALSASEQ)
    return std::unique_ptr<MidiInput>(new AlsaSeqMidiInput());
#else
    return std::unique_ptr<MidiInput>(new NullMidiInput());
#endif
}

std::unique_ptr<MidiInput> createMidiInput()
{
    return createMidiInput(wantedBackend());
}

}  // namespace audio
