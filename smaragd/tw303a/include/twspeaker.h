#ifndef _TWSPEAKER_H_
#define _TWSPEAKER_H_

#include "twcomponent.h"
#include "twresampler.h"
#include "audio/audio_backend.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class twSpeaker
    : public twComponent
{
    Q_OBJECT
private:
    std::unique_ptr<audio::AudioBackend> backend_;
    bool isPlaying_;
    twResampler resampler_;
    std::string outputDeviceId_ = "default";

    // Cycle (loop) playback. When enabled with a valid range, the render
    // callback seamlessly wraps the play cursor from loopEnd_ back to loopStart_
    // (the end frame itself is not played, so the loop is [start, end)). The
    // atomics are written from the UI thread (setCycle) and read on the audio
    // thread, so playback never tears on a mid-buffer toggle.
    std::atomic<bool>     cycleEnabled_{ false };
    std::atomic<offset_t> loopStart_{ 0 };
    std::atomic<offset_t> loopEnd_{ 0 };
    // Output frames produced per input (synth-time) frame consumed; 1.0 when the
    // device opened at the graph rate (resampler is a passthrough). Used to bound
    // a pull so it doesn't overshoot the loop end. Set in startOutput().
    double                rateRatio_ = 1.0;

protected:
    virtual length_t calcOutputTo(sample_t *pDest, length_t length, idx_t idx);

public:
    ~twSpeaker();
    twSpeaker(tw303aEnvironment &);

    virtual void createOutputLatches(void);

    virtual const char *getInputName(idx_t)  const { return nullptr; }
    virtual const char *getOutputName(idx_t) const { return nullptr; }
    virtual idx_t getNInputs()  const { return 2; }
    virtual idx_t getNOutputs() const { return 0; }

    void setBufferSize(length_t) {}

    bool isPlaying();

    // Enable/disable cycle (loop) playback and set its bounds (sample frames in
    // synth time). An empty/inverted range (end <= start) disables cycling. Safe
    // to call at any time, including during playback.
    void setCycle( bool enabled, offset_t startFrame, offset_t endFrame );

    // Output device selection (for a device-picker UI). The id is a backend
    // device id from outputDevices(); "default" / empty means the system
    // default endpoint. Takes effect on the next startOutput().
    void setOutputDevice( const std::string &id );
    const std::string &outputDevice() const { return outputDeviceId_; }
    std::vector<audio::AudioDeviceInfo> outputDevices() const;

public slots:
    void startOutput();
    void stopOutput();
};

#endif
