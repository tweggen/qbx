#include "coreaudio_input.h"

#include <AVFoundation/AVFoundation.h>
#include <cstdint>
#include <cstring>
#include <thread>

namespace audio {

CoreAudioInput::CoreAudioInput() {
    config_.sampleRate = 48000;
    config_.channels = 1;
    config_.bufferFrames = 1024;
    config_.sampleType = twSampleType::Float32;
    circularBuffer_.resize(2 * 48000 * 1);  // 2 seconds at 48kHz
}

CoreAudioInput::~CoreAudioInput() {
    closeDevice();
}

int CoreAudioInput::openDevice(const std::string &deviceId, std::uint32_t preferredRate) {
    if (@available(macOS 10.13, *)) {
        AVAudioEngine *engine = [[AVAudioEngine alloc] init];
        AVAudioInputNode *inputNode = [engine inputNode];

        if (!inputNode) {
            lastError_ = "Failed to create AVAudioEngine input node";
            [engine release];
            return -1;
        }

        // Get the input format (device's native format)
        AVAudioFormat *inputFormat = [inputNode outputFormatForBus:0];
        if (!inputFormat) {
            lastError_ = "Failed to get input node format";
            [engine release];
            return -1;
        }

        config_.sampleRate = static_cast<std::uint32_t>(inputFormat.sampleRate);
        config_.channels = inputFormat.channelCount;

        // Store engine as opaque void pointer
        audioUnit_ = reinterpret_cast<AudioComponentInstance>(engine);

        // Start the engine — this is required for input to flow
        NSError *error = nil;
        if (![engine startAndReturnError:&error]) {
            fprintf(stderr, "coreaudio_input: ERROR starting engine: %s\n",
                    error ? [[error description] cStringUsingEncoding:NSUTF8StringEncoding] : "unknown error");
            fflush(stderr);
            lastError_ = error ? [[error description] cStringUsingEncoding:NSUTF8StringEncoding] : "Failed to start AVAudioEngine";
            [engine release];
            audioUnit_ = nullptr;
            return -1;
        }

        fprintf(stderr, "coreaudio_input: AVAudioEngine initialized at %u Hz, %u channels, engine running\n",
                (unsigned)config_.sampleRate, config_.channels);
        fflush(stderr);

        return 0;
    } else {
        lastError_ = "AVAudioEngine requires macOS 10.13+";
        return -1;
    }
}

int CoreAudioInput::closeDevice() {
    stopCapture();

    if (audioUnit_) {
        AVAudioEngine *engine = reinterpret_cast<AVAudioEngine *>(audioUnit_);
        [engine stop];
        [engine release];
        audioUnit_ = nullptr;
    }

    return 0;
}

int CoreAudioInput::startCapture() {
    if (!audioUnit_) {
        lastError_ = "Audio engine not initialized";
        return -1;
    }

    writePos_.store(0);
    readPos_.store(0);

    if (@available(macOS 10.13, *)) {
        AVAudioEngine *engine = reinterpret_cast<AVAudioEngine *>(audioUnit_);
        AVAudioInputNode *inputNode = [engine inputNode];

        if (!inputNode) {
            lastError_ = "Input node unavailable";
            return -1;
        }

        // Capture 'this' safely for the block
        CoreAudioInput *captureThis = this;

        // Check if engine is running
        if (![engine isRunning]) {
            fprintf(stderr, "coreaudio_input: WARNING — engine not running, attempting to start...\n");
            fflush(stderr);
            NSError *startError = nil;
            if (![engine startAndReturnError:&startError]) {
                fprintf(stderr, "coreaudio_input: ERROR starting engine: %s\n",
                        [[startError description] cStringUsingEncoding:NSUTF8StringEncoding]);
                fflush(stderr);
                return -1;
            }
        }

        // Install tap on input node to capture audio
        AVAudioFormat *format = [inputNode outputFormatForBus:0];
        if (!format) {
            fprintf(stderr, "coreaudio_input: ERROR — could not get input format\n");
            fflush(stderr);
            return -1;
        }

        [inputNode installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer *buffer, AVAudioTime *when) {
            if (captureThis) {
                captureThis->captureAVAudioBuffer((void *)buffer);
            }
        }];

        fprintf(stderr, "coreaudio_input: input tap installed, capture starting...\n");
        fprintf(stderr, "coreaudio_input: engine running=%s, format=%u Hz, %u ch\n",
                [engine isRunning] ? "yes" : "no", (unsigned)format.sampleRate, format.channelCount);
        fflush(stderr);

        isCapturing_ = true;
        return 0;
    }

    lastError_ = "AVAudioEngine not available";
    return -1;
}

int CoreAudioInput::stopCapture() {
    if (!audioUnit_ || !isCapturing_) {
        return 0;
    }

    stopCaptureThread_.store(true);
    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }

    if (@available(macOS 10.13, *)) {
        AVAudioEngine *engine = reinterpret_cast<AVAudioEngine *>(audioUnit_);
        AVAudioInputNode *inputNode = [engine inputNode];
        if (inputNode) {
            // Remove the tap to stop capturing
            [inputNode removeTapOnBus:0];
        }
    }

    isCapturing_ = false;
    return 0;
}

void CoreAudioInput::captureAVAudioBuffer(void *avAudioPCMBuffer) {
    AVAudioPCMBuffer *buffer = (AVAudioPCMBuffer *)avAudioPCMBuffer;
    if (!buffer) return;

    AVAudioFormat *format = buffer.format;
    std::size_t frameCount = buffer.frameLength;
    std::uint32_t channels = format.channelCount;

    // Convert float32 audio to our internal format and buffer
    float * const *floatChannelData = buffer.floatChannelData;

    static int tapCount = 0;
    ++tapCount;

    if (tapCount <= 3) {
        fprintf(stderr, "coreaudio_input: tap #%d — frameCount=%zu, ch=%u, floatChannelData=%p\n",
                tapCount, frameCount, channels, floatChannelData);
        fflush(stderr);
    }

    if (!floatChannelData || !floatChannelData[0]) {
        if (tapCount <= 3) {
            fprintf(stderr, "coreaudio_input: tap #%d — ERROR: no float channel data!\n", tapCount);
            fflush(stderr);
        }
        return;
    }
    float *floatData = floatChannelData[0];  // First channel

    // Diagnostic: check if buffer contains actual audio or just zeros
    if (tapCount <= 3) {
        float maxSample = 0.0f;
        for (std::size_t i = 0; i < std::min(frameCount, size_t(1000)); ++i) {
            float absVal = floatData[i] < 0 ? -floatData[i] : floatData[i];
            if (absVal > maxSample) maxSample = absVal;
        }
        fprintf(stderr, "coreaudio_input: tap #%d — maxSample=%.6f (first 1000 frames)\n",
                tapCount, maxSample);
        fflush(stderr);
    }

    std::unique_lock<std::mutex> lock(bufferMutex_);
    std::size_t wp = writePos_.load();
    std::size_t rp = readPos_.load();

    for (std::size_t i = 0; i < frameCount * channels; ++i) {
        std::size_t nextWp = (wp + 1) % circularBuffer_.size();
        if (nextWp != rp) {
            circularBuffer_[wp] = floatData[i];
            wp = nextWp;
        }
    }

    writePos_.store(wp);
    bufferCV_.notify_one();
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
