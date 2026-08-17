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
    ring_.reset(config_.channels, 16384);
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
        ring_.reset(config_.channels, 16384);

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

        // Query input latency from the AVAudioEngine's input node.
        // AVAudioEngine doesn't provide a direct latency property, but we can estimate
        // based on the input node's buffer size and the configured I/O buffer.
        // For now, estimate as buffer frames (typical device latency is 1 buffer).
        AVAudioInputNode *inNode = [engine inputNode];
        if (inNode && inNode.audioUnit) {
            Float64 latencySecs = 0.0;
            UInt32 propSize = sizeof(latencySecs);
            OSStatus err = AudioUnitGetProperty(inNode.audioUnit,
                                                 kAudioUnitProperty_Latency,
                                                 kAudioUnitScope_Global, 0,
                                                 &latencySecs, &propSize);
            if (err == noErr && latencySecs > 0.0) {
                config_.inputLatencyFrames = static_cast<uint32_t>(latencySecs * config_.sampleRate);
                fprintf(stderr, "coreaudio_input: input latency %.2f ms (%u frames @ %u Hz)\n",
                        latencySecs * 1000.0, config_.inputLatencyFrames, (unsigned)config_.sampleRate);
            } else {
                config_.inputLatencyFrames = 0;
            }
        } else {
            config_.inputLatencyFrames = 0;
        }

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

    ring_.clear();
    ring_.resetStats();
    wakeups_.store(0, std::memory_order_relaxed);

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
    const std::size_t frameCount = buffer.frameLength;
    const std::uint32_t channels = format.channelCount;
    float * const *planes = buffer.floatChannelData;
    if (!planes || !planes[0] || frameCount == 0) return;

    // AVAudioPCMBuffer is PLANAR; the ring is interleaved. The old code copied
    // frameCount * channels samples out of plane 0 alone, i.e. read past the
    // end of the plane for a stereo device and wrote the overrun in as if it
    // were channel data. Interleave properly, and push the WHOLE buffer — a tap
    // block that does not fit is an overrun the ring counts, never a silent
    // truncation.
    const std::uint32_t nch = channels < config_.channels ? channels
                                                          : config_.channels;
    std::vector<float> inter(frameCount * config_.channels, 0.0f);
    for (std::size_t f = 0; f < frameCount; ++f)
        for (std::uint32_t c = 0; c < nch; ++c)
            inter[f * config_.channels + c] = planes[c][f];

    ring_.push(inter.data(), frameCount);
    wakeups_.fetch_add(1, std::memory_order_relaxed);
}

void CoreAudioInput::bufferAudioData(const float *audioData, std::size_t frameCount) {
    if (!audioData || frameCount == 0) return;
    ring_.push(audioData, frameCount);
    wakeups_.fetch_add(1, std::memory_order_relaxed);
}

std::int32_t CoreAudioInput::read(float *interleaved, std::size_t frameCount) {
    // A ring pop, and NON-BLOCKING (proposal 21 L0).
    if (!interleaved) return -1;
    return static_cast<std::int32_t>(ring_.pop(interleaved, frameCount));
}

AudioInputStats CoreAudioInput::stats() const {
    AudioInputStats s;
    s.framesPushed = ring_.framesPushed();
    s.framesPopped = ring_.framesPopped();
    s.overrunFrames = ring_.overrunFrames();
    s.underrunFrames = ring_.underrunFrames();
    s.captureWakeups = wakeups_.load(std::memory_order_relaxed);
    return s;
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
