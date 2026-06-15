#include "coreaudio_input.h"

#include <cstring>

namespace audio {

// AudioQueue input callback — invoked when a buffer is filled with audio data
void audioQueueInputCallback(void *inUserData,
                             AudioQueueRef inAQ,
                             AudioQueueBufferRef inBuffer,
                             const AudioTimeStamp *inStartTime,
                             UInt32 inNumberPacketDescriptions,
                             const AudioStreamPacketDescription *inPacketDescs) {
    CoreAudioInput *pThis = static_cast<CoreAudioInput *>(inUserData);
    if (!pThis || !inBuffer->mAudioData) {
        return;
    }

    // Extract audio from the buffer and buffer it
    const float *audioData = static_cast<const float *>(inBuffer->mAudioData);
    std::size_t frameCount = inBuffer->mAudioDataByteSize / (pThis->getConfig().channels * sizeof(float));

    pThis->bufferAudioData(audioData, frameCount);

    // Re-enqueue the buffer so CoreAudio can fill it again
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);
}

CoreAudioInput::CoreAudioInput() {
    config_.sampleRate = 48000;
    config_.channels = 2;
    config_.bufferFrames = 1024;
    config_.sampleType = twSampleType::Float32;
    // Allocate circular buffer: 2 seconds at max sample rate (48kHz) * 2 channels * float
    circularBuffer_.resize(2 * 48000 * 2);
}

CoreAudioInput::~CoreAudioInput() {
    closeDevice();
}

int CoreAudioInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    if (inputQueue_) {
        lastError_ = "Device already open";
        return -1;
    }

    // Get the default input device (or specified device)
    AudioDeviceID inputDeviceID = 0;
    UInt32 dataSize = sizeof(inputDeviceID);
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultInputDevice,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};

    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                                  &dataSize, &inputDeviceID);
    if (status != noErr || inputDeviceID == 0) {
        lastError_ = "Failed to get default input device";
        return -1;
    }

    // Get device's native sample rate and channels
    AudioStreamBasicDescription deviceFormat;
    dataSize = sizeof(deviceFormat);
    addr.mSelector = kAudioDevicePropertyStreamFormat;
    addr.mScope = kAudioObjectPropertyScopeInput;
    addr.mElement = kAudioObjectPropertyElementMain;

    status = AudioObjectGetPropertyData(inputDeviceID, &addr, 0, nullptr, &dataSize, &deviceFormat);
    if (status != noErr) {
        lastError_ = "Failed to get device format";
        return -1;
    }

    // Set up audio format for the queue (request specific rate if given, else use device rate)
    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof(format));
    format.mSampleRate = preferredRate > 0 ? preferredRate : deviceFormat.mSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = sizeof(float) * deviceFormat.mChannelsPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * deviceFormat.mChannelsPerFrame;
    format.mChannelsPerFrame = deviceFormat.mChannelsPerFrame;
    format.mBitsPerChannel = 32;

    config_.sampleRate = static_cast<std::uint32_t>(format.mSampleRate);
    config_.channels = format.mChannelsPerFrame;

    // Create the audio queue
    status = AudioQueueNewInput(&format, audioQueueInputCallback, this,
                                nullptr, nullptr, 0, &inputQueue_);
    if (status != noErr) {
        lastError_ = "Failed to create audio queue";
        return -1;
    }

    // Set the input device on the queue
    status = AudioQueueSetProperty(inputQueue_, kAudioQueueProperty_CurrentDevice,
                                   &inputDeviceID, sizeof(inputDeviceID));
    if (status != noErr) {
        lastError_ = "Failed to set input device on queue";
        AudioQueueDispose(inputQueue_, true);
        inputQueue_ = nullptr;
        return -1;
    }

    // Create and enqueue buffers
    const UInt32 bufferByteSize = config_.bufferFrames * config_.channels * sizeof(float);
    for (int i = 0; i < 2; ++i) {
        status = AudioQueueAllocateBuffer(inputQueue_, bufferByteSize, &queueBuffers_[i]);
        if (status != noErr) {
            lastError_ = "Failed to allocate audio queue buffer";
            AudioQueueDispose(inputQueue_, true);
            inputQueue_ = nullptr;
            return -1;
        }

        status = AudioQueueEnqueueBuffer(inputQueue_, queueBuffers_[i], 0, nullptr);
        if (status != noErr) {
            lastError_ = "Failed to enqueue audio queue buffer";
            AudioQueueDispose(inputQueue_, true);
            inputQueue_ = nullptr;
            return -1;
        }
    }

    return 0;
}

int CoreAudioInput::closeDevice() {
    stopCapture();

    if (inputQueue_) {
        AudioQueueDispose(inputQueue_, true);
        inputQueue_ = nullptr;
    }

    return 0;
}

int CoreAudioInput::startCapture() {
    if (!inputQueue_) {
        lastError_ = "Audio queue not initialized";
        return -1;
    }

    // Reset buffer
    writePos_.store(0);
    readPos_.store(0);

    OSStatus status = AudioQueueStart(inputQueue_, nullptr);
    if (status != noErr) {
        lastError_ = "Failed to start audio queue";
        return -1;
    }

    isCapturing_ = true;
    return 0;
}

int CoreAudioInput::stopCapture() {
    if (!inputQueue_ || !isCapturing_) {
        return 0;
    }

    AudioQueueStop(inputQueue_, true);
    isCapturing_ = false;
    return 0;
}

void CoreAudioInput::bufferAudioData(const float *audioData, std::size_t frameCount) {
    if (!audioData || frameCount == 0) {
        return;
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

    // Wait for data with timeout
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

    // TODO: Enumerate CoreAudio input devices using AudioObjectFind and property queries
    // For now, just return the default device
    devices.push_back({"default", "Default Input", config_.channels});

    return devices;
}

const char *CoreAudioInput::errorMessage() const {
    return lastError_.c_str();
}

}  // namespace audio
