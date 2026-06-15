#include "coreaudio_input.h"

#include <cstdint>
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
    if (!pThis || !inBuffer || !inBuffer->mAudioData || inBuffer->mAudioDataByteSize == 0) {
        if (inBuffer) {
            AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);
        }
        return;
    }

    // Device provides int16 PCM. Convert to float for internal use.
    const int16_t *int16Data = static_cast<const int16_t *>(inBuffer->mAudioData);
    std::size_t sampleCount = inBuffer->mAudioDataByteSize / sizeof(int16_t);

    // Debug: check raw int16 values
    static int debugCount = 0;
    if (++debugCount <= 3) {
        int16_t maxInt = 0;
        for (std::size_t i = 0; i < std::min(sampleCount, size_t(100)); ++i) {
            int16_t absVal = int16Data[i] < 0 ? -int16Data[i] : int16Data[i];
            if (absVal > maxInt) maxInt = absVal;
        }
        fprintf(stderr, "coreaudio_input callback #%d: sampleCount=%zu, mAudioDataByteSize=%u, maxInt16=%d\n",
                debugCount, sampleCount, (unsigned)inBuffer->mAudioDataByteSize, (int)maxInt);
        fflush(stderr);
    }

    // Convert int16 to float and buffer (int16 range: -32768 to 32767)
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

    // Simple approach: request 16-bit PCM at the preferred rate (or 48kHz default)
    // and let AudioQueue use the system's default input device and handle format conversion
    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof(format));
    format.mSampleRate = preferredRate > 0 ? preferredRate : 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = 2; // 16-bit = 2 bytes
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2;
    format.mChannelsPerFrame = 1;
    format.mBitsPerChannel = 16;

    fprintf(stderr, "coreaudio_input: requesting %u Hz, 1 channel, 16-bit PCM\n", (unsigned)format.mSampleRate);
    fflush(stderr);

    config_.sampleRate = static_cast<std::uint32_t>(format.mSampleRate);
    config_.channels = format.mChannelsPerFrame;

    // Create audio queue with default system input device
    OSStatus status = AudioQueueNewInput(&format, audioQueueInputCallback, this,
                                         nullptr, nullptr, 0, &inputQueue_);
    if (status != noErr) {
        lastError_ = "Failed to create audio queue";
        fprintf(stderr, "coreaudio_input: AudioQueueNewInput failed with status %d\n", (int)status);
        return -1;
    }

    // Don't try to set a specific device - use system default (the microphone user selected)

    // Create and enqueue buffers
    const UInt32 bufferByteSize = config_.bufferFrames * config_.channels * sizeof(int16_t);
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

    fprintf(stderr, "coreaudio_input: device opened, queue created\n");
    fflush(stderr);
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

    // Diagnostic: check if buffer contains actual audio or silence
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
