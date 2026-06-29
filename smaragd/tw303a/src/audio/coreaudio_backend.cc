#include "audio/coreaudio_backend.h"

#include "twconvert.h"
#include "twsyslog.h"

// Include CoreAudio headers
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Helper: get a device's nominal sample rate.
uint32_t getDeviceNominalRate(AudioDeviceID device)
{
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    Float64 rate = 0.0;
    uint32_t size = sizeof(rate);
    OSStatus err = AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &rate);
    if (err == noErr) {
        return static_cast<uint32_t>(rate);
    }
    return 44100;  // fallback
}

// Helper: get a device's friendly name.
std::string getDeviceName(AudioDeviceID device)
{
    AudioObjectPropertyAddress addr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };

    CFStringRef nameCF = nullptr;
    uint32_t size = sizeof(nameCF);
    OSStatus err = AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &nameCF);
    if (err == noErr && nameCF) {
        const char *utf8 = CFStringGetCStringPtr(nameCF, kCFStringEncodingUTF8);
        if (utf8) {
            std::string name(utf8);
            CFRelease(nameCF);
            return name;
        }
        CFRelease(nameCF);
    }
    return "Unknown Device";
}

// Helper: convert CoreAudio sample rate / format to our twSampleType.
twSampleType detectSampleType(const AudioStreamBasicDescription &desc)
{
    if (desc.mFormatFlags & kAudioFormatFlagIsFloat) {
        if (desc.mBitsPerChannel == 32) return twSampleType::Float32;
        if (desc.mBitsPerChannel == 64) return twSampleType::Float64;
    } else {
        if (desc.mBitsPerChannel == 16) return twSampleType::Int16;
        if (desc.mBitsPerChannel == 32) return twSampleType::Int32;
    }
    return twSampleType::Float32;  // fallback
}

}  // namespace

namespace audio {

CoreAudioBackend::CoreAudioBackend()
{
    config_.sampleRate = 44100;
    config_.channels = 2;
    config_.bufferFrames = 1024;
    config_.periodFrames = 256;
    config_.sampleType = twSampleType::Float32;
}

CoreAudioBackend::~CoreAudioBackend()
{
    if (running_.load()) {
        stopOutput();
    }
    closeDevice();
}

int CoreAudioBackend::openDevice(const std::string &deviceId, std::uint32_t preferredRate)
{
    if (outputUnit_) {
        syslog(LOG_WARNING, "CoreAudioBackend::openDevice: already open");
        return 0;
    }

    OSStatus err = noErr;

    // Determine which device to open.
    AudioDeviceID device = kAudioObjectUnknown;

    if (!deviceId.empty() && deviceId != "default") {
        // Try to parse the device ID as a number (AudioDeviceID).
        try {
            device = static_cast<AudioDeviceID>(std::stoul(deviceId));
        } catch (...) {
            syslog(LOG_ERR, "CoreAudioBackend: invalid device ID: %s", deviceId.c_str());
            return -1;
        }
    } else {
        // Use the default output device.
        AudioObjectPropertyAddress addr = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };

        uint32_t size = sizeof(device);
        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                         &size, &device);
        if (err != noErr || device == kAudioObjectUnknown) {
            syslog(LOG_ERR, "CoreAudioBackend: AudioObjectGetPropertyData(default device) failed: %d",
                   (int)err);
            return -1;
        }
    }

    deviceId_ = device;

    // Find the DefaultOutput audio unit (standard macOS speaker output).
    AudioComponentDescription desc = {
        kAudioUnitType_Output,
        kAudioUnitSubType_DefaultOutput,
        kAudioUnitManufacturer_Apple,
        0, 0
    };

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        syslog(LOG_ERR, "CoreAudioBackend: AudioComponentFindNext(DefaultOutput) failed");
        return -1;
    }

    // Create the audio unit instance.
    ::AudioUnit tempUnit = nullptr;
    err = AudioComponentInstanceNew(comp, reinterpret_cast<AudioComponentInstance *>(&tempUnit));
    if (err != noErr) {
        syslog(LOG_ERR, "CoreAudioBackend: AudioComponentInstanceNew failed: %d", (int)err);
        return -1;
    }
    outputUnit_ = tempUnit;

    // Query the device's format and capabilities via AudioHardware APIs.
    // This is more reliable than querying the AudioUnit's format.
    config_.sampleRate = getDeviceNominalRate(device);
    config_.channels = 2;  // Default to stereo; query if needed

    // Query the device's stream format (if available).
    // For now, we assume the device supports float32 interleaved, and the
    // speaker resampler bridges any rate mismatch. Full device format
    // negotiation is future work.
    config_.sampleType = twSampleType::Float32;

    // If a preferred rate was requested and the device can support it, try to use it.
    // For now, we accept the device's format as-is (the speaker's resampler bridges
    // any mismatch). Exclusive-mode rate selection is future work.
    if (preferredRate != 0 && preferredRate != config_.sampleRate) {
        syslog(LOG_WARNING,
               "CoreAudioBackend: device opened at %u Hz, project requested %u Hz; resampling",
               (unsigned)config_.sampleRate, (unsigned)preferredRate);
    }

    // Set the output unit's format to what the app produces (stereo float32 at device rate).
    // Use the most basic format flags for compatibility.
    AudioStreamBasicDescription appFormat = {0};
    appFormat.mSampleRate = config_.sampleRate;
    appFormat.mFormatID = kAudioFormatLinearPCM;
    appFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;  // Interleaved float
    appFormat.mBytesPerPacket = 4 * config_.channels;  // float32 * num channels
    appFormat.mFramesPerPacket = 1;
    appFormat.mBytesPerFrame = 4 * config_.channels;
    appFormat.mChannelsPerFrame = config_.channels;
    appFormat.mBitsPerChannel = 32;

    ::AudioUnit au = reinterpret_cast<::AudioUnit>(outputUnit_);

    // Set format on INPUT scope (where we provide data to the unit).
    err = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &appFormat, sizeof(appFormat));
    if (err != noErr) {
        syslog(LOG_ERR, "CoreAudioBackend: AudioUnitSetProperty(format) failed: %d", (int)err);
        AudioComponentInstanceDispose(reinterpret_cast<::AudioComponentInstance>(au));
        outputUnit_ = nullptr;
        return -1;
    }

    // Set format on OUTPUT scope (often read-only, but try anyway).
    err = AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 0, &appFormat, sizeof(appFormat));
    if (err != noErr) {
        syslog(LOG_DEBUG, "CoreAudioBackend: OUTPUT scope format not settable (expected on some units): %d", (int)err);
    }

    // Install the render callback (invoked when the unit needs audio data).
    AURenderCallbackStruct callbackStruct;
    callbackStruct.inputProc = reinterpret_cast<AURenderCallback>(CoreAudioBackend::renderCallback_);
    callbackStruct.inputProcRefCon = this;

    err = AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &callbackStruct, sizeof(callbackStruct));
    if (err != noErr) {
        syslog(LOG_ERR, "CoreAudioBackend: AudioUnitSetProperty(SetRenderCallback) failed: %d", (int)err);
        AudioComponentInstanceDispose(reinterpret_cast<::AudioComponentInstance>(au));
        outputUnit_ = nullptr;
        return -1;
    }

    // Initialize the audio unit.
    err = AudioUnitInitialize(au);
    if (err != noErr) {
        syslog(LOG_ERR, "CoreAudioBackend: AudioUnitInitialize failed: %d", (int)err);
        AudioComponentInstanceDispose(reinterpret_cast<::AudioComponentInstance>(au));
        outputUnit_ = nullptr;
        return -1;
    }

    // Query output latency from the audio unit (device + processing latency).
    Float64 latencySecs = 0.0;
    uint32_t propSize = sizeof(latencySecs);
    err = AudioUnitGetProperty(au, kAudioUnitProperty_Latency,
                               kAudioUnitScope_Global, 0,
                               &latencySecs, &propSize);
    if (err == noErr && latencySecs > 0.0) {
        config_.outputLatencyFrames = static_cast<uint32_t>(latencySecs * config_.sampleRate);
        syslog(LOG_INFO,
               "CoreAudioBackend: output latency %.2f ms (%u frames @ %u Hz)",
               latencySecs * 1000.0, config_.outputLatencyFrames, config_.sampleRate);
    } else {
        config_.outputLatencyFrames = 0;
    }

    floatScratch_.resize(config_.bufferFrames * config_.channels);

    syslog(LOG_INFO, "CoreAudioBackend: opened device %u at %u Hz, %u channels",
           (unsigned)device, (unsigned)config_.sampleRate, (unsigned)config_.channels);

    return 0;
}

int CoreAudioBackend::closeDevice()
{
    if (!outputUnit_) return 0;

    ::AudioUnit au = reinterpret_cast<::AudioUnit>(outputUnit_);
    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(reinterpret_cast<::AudioComponentInstance>(au));
    outputUnit_ = nullptr;

    return 0;
}

int CoreAudioBackend::startOutput()
{
    if (!outputUnit_) {
        syslog(LOG_ERR, "CoreAudioBackend::startOutput: no device open");
        return -1;
    }

    OSStatus err = AudioOutputUnitStart(reinterpret_cast<::AudioUnit>(outputUnit_));
    if (err != noErr) {
        syslog(LOG_ERR, "CoreAudioBackend: AudioOutputUnitStart failed: %d", (int)err);
        return -1;
    }

    running_.store(true);
    return 0;
}

int CoreAudioBackend::stopOutput()
{
    if (!outputUnit_) return 0;

    running_.store(false);
    // Cast opaque pointer to AudioUnit for the CoreAudio call
    OSStatus err = AudioOutputUnitStop(reinterpret_cast<::AudioUnit>(outputUnit_));
    if (err != noErr) {
        syslog(LOG_ERR, "CoreAudioBackend: AudioOutputUnitStop failed: %d", (int)err);
        return -1;
    }

    return 0;
}

std::vector<std::uint32_t> CoreAudioBackend::supportedRates() const
{
    if (!outputUnit_) return {};

    // Return the device's current mix rate. Full rate enumeration would require
    // querying the device's available stream configurations, which is complex;
    // for now return a single rate (the speaker's resampler bridges mismatches).
    uint32_t rate = config_.sampleRate;
    return rate > 0 ? std::vector<uint32_t>{rate} : std::vector<uint32_t>{};
}

std::vector<AudioDeviceInfo> CoreAudioBackend::enumerateDevices() const
{
    std::vector<AudioDeviceInfo> devices;

    // Enumerate all output devices.
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };

    uint32_t size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr,
                                                  0, nullptr, &size);
    if (err != noErr) return devices;

    std::vector<AudioDeviceID> deviceList(size / sizeof(AudioDeviceID));
    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                     &size, deviceList.data());
    if (err != noErr) return devices;

    for (AudioDeviceID devId : deviceList) {
        // Check if this device has output streams.
        addr.mSelector = kAudioDevicePropertyStreamConfiguration;
        addr.mScope = kAudioObjectPropertyScopeOutput;

        AudioBufferList *bufList = nullptr;
        size = 0;
        err = AudioObjectGetPropertyDataSize(devId, &addr, 0, nullptr, &size);
        if (err != noErr || size == 0) continue;

        bufList = reinterpret_cast<AudioBufferList *>(malloc(size));
        err = AudioObjectGetPropertyData(devId, &addr, 0, nullptr, &size, bufList);
        bool hasOutput = (err == noErr && bufList->mNumberBuffers > 0);
        free(bufList);

        if (!hasOutput) continue;

        std::string name = getDeviceName(devId);
        devices.push_back(AudioDeviceInfo{
            std::to_string(devId),
            name
        });
    }

    return devices;
}

int CoreAudioBackend::renderCallback_(void *refCon,
                                      void *flags,
                                      const void *ts,
                                      unsigned int /*busNum*/,
                                      unsigned int frames,
                                      void *buffers_)
{
    // Invoked by CoreAudio when it needs audio data.
    CoreAudioBackend *self = reinterpret_cast<CoreAudioBackend *>(refCon);
    if (self) {
        self->renderOnce_(frames, buffers_);
    } else {
        // Silence the output if no instance.
        if (buffers_) {
            auto *buffers = reinterpret_cast<AudioBufferList *>(buffers_);
            for (unsigned i = 0; i < buffers->mNumberBuffers; ++i) {
                if (buffers->mBuffers[i].mData && buffers->mBuffers[i].mDataByteSize) {
                    std::memset(buffers->mBuffers[i].mData, 0, buffers->mBuffers[i].mDataByteSize);
                }
            }
        }
    }
    return 0;  // noErr
}

void CoreAudioBackend::renderOnce_(unsigned int frames, void *buffers_)
{
    if (!buffers_) return;
    auto *buffers = reinterpret_cast<AudioBufferList *>(buffers_);

    if (!callback_) {
        // No callback: silence the buffers.
        for (unsigned i = 0; i < buffers->mNumberBuffers; ++i) {
            if (buffers->mBuffers[i].mData && buffers->mBuffers[i].mDataByteSize) {
                std::memset(buffers->mBuffers[i].mData, 0, buffers->mBuffers[i].mDataByteSize);
            }
        }
        return;
    }

    // Clamp frames to the scratch buffer size to prevent overflow.
    // If CoreAudio requests more frames than we can process in one callback,
    // we process what we can and pad the rest with silence.
    unsigned int framesToProcess = frames;
    if (framesToProcess > config_.bufferFrames) {
        framesToProcess = config_.bufferFrames;
    }

    // Pull floats from the synthesizer (up to framesToProcess).
    size_t filled = callback_(floatScratch_.data(), framesToProcess, config_.channels);

    // Copy the float data to the audio buffers.
    const size_t frameBytes = config_.channels * sizeof(float);
    for (unsigned i = 0; i < buffers->mNumberBuffers; ++i) {
        if (buffers->mBuffers[i].mData) {
            // Copy filled data.
            if (filled > 0) {
                std::memcpy(buffers->mBuffers[i].mData, floatScratch_.data(),
                            filled * frameBytes);
            }
            // Silence the rest of the requested frames.
            if (filled < frames) {
                std::memset(reinterpret_cast<uint8_t *>(buffers->mBuffers[i].mData) + filled * frameBytes,
                            0, (frames - filled) * frameBytes);
            }
        }
    }
}

}  // namespace audio
