#include "coreaudio_input.h"

#include <AudioToolbox/AudioToolbox.h>
#include <cstdint>
#include <cstring>

namespace audio {

// Static callback for input render — receives audio from the device
static OSStatus inputRenderCallback(void *inRefCon,
                                    AudioUnitRenderActionFlags *ioActionFlags,
                                    const AudioTimeStamp *inTimeStamp,
                                    UInt32 inBusNumber,
                                    UInt32 inNumberFrames,
                                    AudioBufferList *ioData) {
    CoreAudioInput *pThis = static_cast<CoreAudioInput *>(inRefCon);
    if (!pThis || !ioData || ioData->mNumberBuffers == 0) {
        return noErr;
    }

    // ioData contains the captured audio from the device
    const AudioBuffer &buffer = ioData->mBuffers[0];
    if (!buffer.mData || buffer.mDataByteSize == 0) {
        return noErr;
    }

    // Audio is in the device's native format (int16). Convert to float.
    const int16_t *int16Data = static_cast<const int16_t *>(buffer.mData);
    std::size_t sampleCount = buffer.mDataByteSize / sizeof(int16_t);

    // Debug: check raw int16 values
    static int debugCount = 0;
    if (++debugCount <= 3) {
        int16_t maxInt = 0;
        for (std::size_t i = 0; i < std::min(sampleCount, size_t(100)); ++i) {
            int16_t absVal = int16Data[i] < 0 ? -int16Data[i] : int16Data[i];
            if (absVal > maxInt) maxInt = absVal;
        }
        fprintf(stderr, "coreaudio_input callback #%d: sampleCount=%zu, mAudioDataByteSize=%u, maxInt16=%d\n",
                debugCount, sampleCount, (unsigned)buffer.mDataByteSize, (int)maxInt);
        fflush(stderr);
    }

    // Convert int16 to float and buffer
    static std::vector<float> floatBuffer;
    floatBuffer.clear();
    floatBuffer.reserve(sampleCount);

    const float scale = 1.0f / 32768.0f;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        floatBuffer.push_back(int16Data[i] * scale);
    }

    // Buffer the converted audio
    std::size_t frameCount = sampleCount / pThis->getConfig().channels;
    if (frameCount > 0) {
        pThis->bufferAudioData(floatBuffer.data(), frameCount);
    }

    return noErr;
}

CoreAudioInput::CoreAudioInput() {
    config_.sampleRate = 48000;
    config_.channels = 1;  // Start with mono input
    config_.bufferFrames = 1024;
    config_.sampleType = twSampleType::Float32;
    // Allocate circular buffer: 2 seconds at max sample rate (48kHz) * 1 channel * float
    circularBuffer_.resize(2 * 48000 * 1);
}

CoreAudioInput::~CoreAudioInput() {
    closeDevice();
}

int CoreAudioInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    if (audioUnit_) {
        lastError_ = "Device already open";
        return -1;
    }

    // Create HAL input audio unit (same approach as playback)
    AudioComponentDescription desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) {
        lastError_ = "Failed to find HAL output component";
        return -1;
    }

    OSStatus status = AudioComponentInstanceNew(comp, &audioUnit_);
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

    // Disable output scope (we only want input)
    enableIO = 0;
    AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_EnableIO,
                        kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));

    // Set input render callback
    AURenderCallbackStruct callback;
    callback.inputProc = inputRenderCallback;
    callback.inputProcRefCon = this;

    status = AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_SetRenderCallback,
                                 kAudioUnitScope_Output, 1, &callback, sizeof(callback));
    if (status != noErr) {
        lastError_ = "Failed to set input render callback";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    // Set up audio format: 16-bit PCM at requested rate
    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof(format));
    format.mSampleRate = preferredRate > 0 ? preferredRate : 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 2 * 1;  // 16-bit mono
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2 * 1;
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 16;

    status = AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Output, 1, &format, sizeof(format));
    if (status != noErr) {
        lastError_ = "Failed to set audio format";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    config_.sampleRate = static_cast<std::uint32_t>(format.mSampleRate);
    config_.channels = format.mChannelsPerFrame;

    // Initialize the audio unit
    status = AudioUnitInitialize(audioUnit_);
    if (status != noErr) {
        lastError_ = "Failed to initialize audio unit";
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        return -1;
    }

    fprintf(stderr, "coreaudio_input: HAL input unit opened at %u Hz\n",
            (unsigned)config_.sampleRate);
    fflush(stderr);

    return 0;
}

int CoreAudioInput::closeDevice() {
    stopCapture();

    if (audioUnit_) {
        AudioUnitUninitialize(audioUnit_);
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
    }

    return 0;
}

int CoreAudioInput::startCapture() {
    if (!audioUnit_) {
        lastError_ = "Audio unit not initialized";
        return -1;
    }

    writePos_.store(0);
    readPos_.store(0);

    OSStatus status = AudioOutputUnitStart(audioUnit_);
    if (status != noErr) {
        lastError_ = "Failed to start audio unit";
        return -1;
    }

    fprintf(stderr, "coreaudio_input: capture started\n");
    fflush(stderr);

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

void CoreAudioInput::bufferAudioData(const float *audioData, std::size_t frameCount) {
    if (!audioData || frameCount == 0) {
        return;
    }

    // Diagnostic: check if buffer contains actual audio
    static int checkCount = 0;
    if (++checkCount <= 3) {
        float maxSample = 0.0f;
        std::size_t checkSamples = std::min(frameCount * config_.channels, size_t(4096));
        for (std::size_t i = 0; i < checkSamples; ++i) {
            float absVal = audioData[i] < 0 ? -audioData[i] : audioData[i];
            if (absVal > maxSample) maxSample = absVal;
        }
        fprintf(stderr, "coreaudio_input: bufferAudioData #%d: frameCount=%zu, channels=%u, maxSample=%.6f\n",
                checkCount, frameCount, config_.channels, maxSample);
        fflush(stderr);
    }

    std::unique_lock<std::mutex> lock(bufferMutex_);
    std::size_t wp = writePos_.load();
    std::size_t rp = readPos_.load();
    std::size_t inSamples = frameCount * config_.channels;

    for (std::size_t i = 0; i < inSamples; ++i) {
        std::size_t nextWp = (wp + 1) % circularBuffer_.size();
        if (nextWp != rp) {
            circularBuffer_[wp] = audioData[i];
            wp = nextWp;
        }
    }

    writePos_.store(wp);
    bufferCV_.notify_one();
}

std::int32_t CoreAudioInput::read(float *interleaved, std::size_t frameCount) {
    if (!interleaved || frameCount == 0) {
        return 0;
    }

    std::unique_lock<std::mutex> lock(bufferMutex_);

    std::size_t rp = readPos_.load();
    std::size_t wp = writePos_.load();
    std::size_t framesAvailable = (wp >= rp) ? (wp - rp) : (circularBuffer_.size() - rp + wp);

    if (framesAvailable < frameCount && isCapturing_) {
        bufferCV_.wait_for(lock, std::chrono::milliseconds(100));
        rp = readPos_.load();
        wp = writePos_.load();
        framesAvailable = (wp >= rp) ? (wp - rp) : (circularBuffer_.size() - rp + wp);
    }

    std::size_t framesCopied = std::min(frameCount, framesAvailable);
    std::size_t samplesCopied = 0;

    for (std::size_t i = 0; i < framesCopied; ++i) {
        for (std::uint32_t c = 0; c < config_.channels; ++c) {
            std::size_t idx = (rp + i) % circularBuffer_.size();
            interleaved[samplesCopied++] = circularBuffer_[idx];
        }
    }

    readPos_.store((rp + framesCopied) % circularBuffer_.size());

    return static_cast<std::int32_t>(framesCopied);
}

const AudioInputConfig &CoreAudioInput::getConfig() const {
    return config_;
}

std::vector<AudioInputDeviceInfo> CoreAudioInput::listDevices() const {
    std::vector<AudioInputDeviceInfo> devices;
    devices.push_back({"default", "Default Input", config_.channels});
    return devices;
}

const char *CoreAudioInput::errorMessage() const {
    return lastError_.c_str();
}

}  // namespace audio
