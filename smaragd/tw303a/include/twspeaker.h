#ifndef _TWSPEAKER_H_
#define _TWSPEAKER_H_

#include "twcomponent.h"
#include "twresampler.h"
#include "audio/audio_backend.h"

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
