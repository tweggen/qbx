#include "tw/devices/keyboard_midi.h"

#include "tw/devices/midi_out_scheduler.h"

#include <algorithm>

namespace audio {

namespace {
std::atomic<KeyboardMidiInput *> g_activeKeyboard{ nullptr };
}

KeyboardMidiInput::KeyboardMidiInput()
{
    g_activeKeyboard.store(this, std::memory_order_release);
}

KeyboardMidiInput::~KeyboardMidiInput()
{
    KeyboardMidiInput *self = this;
    g_activeKeyboard.compare_exchange_strong(self, nullptr,
                                             std::memory_order_acq_rel);
}

KeyboardMidiInput *KeyboardMidiInput::active()
{
    return g_activeKeyboard.load(std::memory_order_acquire);
}

int KeyboardMidiInput::open(const std::string & /*portId*/)
{
    open_.store(true, std::memory_order_release);
    return 0;
}

int KeyboardMidiInput::close()
{
    open_.store(false, std::memory_order_release);
    return 0;
}

std::vector<MidiPortInfo> KeyboardMidiInput::listPorts() const
{
    return { MidiPortInfo{ kPortId, kPortName, false } };
}

bool KeyboardMidiInput::createVirtualPort(const std::string & /*name*/)
{
    // There is nothing to create: this port IS in-process. Saying true would
    // claim a capability the platform half of the interface means something
    // else by (devices inv. 18).
    return false;
}

void KeyboardMidiInput::setCallback(MidiInputCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    cb_ = std::move(cb);
}

void KeyboardMidiInput::inject(const std::uint8_t *bytes, std::size_t size,
                               std::int64_t hostTimeNs)
{
    if (!bytes || size == 0) return;

    MidiInputCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_.load(std::memory_order_acquire)) return;
        cb = cb_;         // copied, then called with NO lock held - the same
    }                     // re-entrancy rule CaptureMidiInput::inject states.
    if (cb) cb(bytes, size, hostTimeNs ? hostTimeNs : MidiOutScheduler::hostNowNs());
}

void KeyboardMidiInput::noteOn(int key, int velocity, int channel)
{
    const std::uint8_t ch = (std::uint8_t) std::max(0, std::min(15, channel));
    const std::uint8_t k  = (std::uint8_t) std::max(0, std::min(127, key));
    const std::uint8_t v  = (std::uint8_t) std::max(0, std::min(127, velocity));
    const std::uint8_t msg[3] = { (std::uint8_t) (0x90 | ch), k, v };
    inject(msg, 3, 0);
}

void KeyboardMidiInput::noteOff(int key, int channel)
{
    const std::uint8_t ch = (std::uint8_t) std::max(0, std::min(15, channel));
    const std::uint8_t k  = (std::uint8_t) std::max(0, std::min(127, key));
    const std::uint8_t msg[3] = { (std::uint8_t) (0x80 | ch), k, 0 };
    inject(msg, 3, 0);
}

}  // namespace audio
