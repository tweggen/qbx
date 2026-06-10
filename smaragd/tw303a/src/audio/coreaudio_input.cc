#include "coreaudio_input.h"

#include <cstring>
#include <sstream>

namespace audio {

CoreAudioInput::CoreAudioInput() {
    config_.sampleRate = 48000;
    config_.channels = 2;
    config_.bufferFrames = 1024;
    config_.sampleType = twSampleType::Float32;
}

CoreAudioInput::~CoreAudioInput() {
    closeDevice();
}

int CoreAudioInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    OSStatus status = noErr;

    // Get default input device if "default" requested
    if (deviceId == "default") {
        UInt32 dataSize = sizeof(AudioDeviceID);
        AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultInputDevice,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};

        status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                             &dataSize, &inputDeviceID_);
        if (status != noErr) {
            lastError_ = "Failed to get default input device";
            return -1;
        }
    } else {
        // TODO: Parse device ID string and get device
        // For now, use default
        UInt32 dataSize = sizeof(AudioDeviceID);
        AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultInputDevice,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};

        status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                             &dataSize, &inputDeviceID_);
        if (status != noErr) {
            lastError_ = "Failed to get input device";
            return -1;
        }
    }

    // Create audio unit for HAL input
    AudioComponentDescription desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        lastError_ = "Failed to find audio component";
        return -1;
    }

    status = AudioComponentInstanceNew(comp, &audioUnit_);
    if (status != noErr) {
        lastError_ = "Failed to create audio unit instance";
        return -1;
    }

    // Enable input on the audio unit
    UInt32 enableIO = 1;
    status = AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_EnableIO,
                                   kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    if (status != noErr) {
        lastError_ = "Failed to enable input on audio unit";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    // Disable output
    enableIO = 0;
    AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                         0, &enableIO, sizeof(enableIO));

    // Set input device
    status = AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Input, 1, &inputDeviceID_,
                                   sizeof(inputDeviceID_));
    if (status != noErr) {
        lastError_ = "Failed to set input device";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    // Get device format and set it on the audio unit
    AudioStreamBasicDescription streamFormat;
    UInt32 dataSize = sizeof(streamFormat);

    AudioObjectPropertyAddress addr = {kAudioDevicePropertyStreamFormat, kAudioObjectPropertyScopeInput,
                                        kAudioObjectPropertyElementMain};
    status = AudioObjectGetPropertyData(inputDeviceID_, &addr, 0, nullptr, &dataSize,
                                         &streamFormat);
    if (status != noErr) {
        lastError_ = "Failed to get device stream format";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    // Override sample rate if preferred
    if (preferredRate > 0) {
        streamFormat.mSampleRate = preferredRate;
    }

    config_.sampleRate = static_cast<std::uint32_t>(streamFormat.mSampleRate);
    config_.channels = streamFormat.mChannelsPerFrame;

    // Set the format on the audio unit
    status = AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                   1, &streamFormat, sizeof(streamFormat));
    if (status != noErr) {
        lastError_ = "Failed to set stream format on audio unit";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    // Initialize audio unit
    status = AudioUnitInitialize(audioUnit_);
    if (status != noErr) {
        lastError_ = "Failed to initialize audio unit";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    return 0;
}

int CoreAudioInput::closeDevice() {
    stopCapture();

    if (audioUnit_) {
        AudioUnitUninitialize(audioUnit_);
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
    }

    inputDeviceID_ = 0;
    return 0;
}

int CoreAudioInput::startCapture() {
    if (!audioUnit_) {
        lastError_ = "Audio unit not initialized";
        return -1;
    }

    OSStatus status = AudioOutputUnitStart(audioUnit_);
    if (status != noErr) {
        lastError_ = "Failed to start audio unit";
        return -1;
    }

    isCapturing_ = true;
    return 0;
}

int CoreAudioInput::stopCapture() {
    if (!audioUnit_ || !isCapturing_) {
        return 0;
    }

    AudioOutputUnitStop(audioUnit_);
    isCapturing_ = false;
    return 0;
}

std::int32_t CoreAudioInput::read(float *interleaved, std::size_t frameCount) {
    // TODO: Implement actual audio reading from CoreAudio
    // This would require setting up an input callback on the audio unit
    // For now, return silence
    if (interleaved) {
        std::memset(interleaved, 0, frameCount * config_.channels * sizeof(float));
    }
    return static_cast<std::int32_t>(frameCount);
}

const AudioInputConfig &CoreAudioInput::getConfig() const {
    return config_;
}

std::vector<AudioInputDeviceInfo> CoreAudioInput::listDevices() const {
    std::vector<AudioInputDeviceInfo> devices;

    // TODO: Enumerate CoreAudio input devices using AudioObjectFind and property queries
    // For now, just return the default device
    devices.push_back({"default", "Default Input", config_.channels});

    return devices;
}

const char *CoreAudioInput::errorMessage() const {
    return lastError_.c_str();
}

}  // namespace audio
