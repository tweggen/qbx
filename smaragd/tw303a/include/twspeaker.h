#ifndef _TWSPEAKER_H_
#define _TWSPEAKER_H_

#include "twcomponent.h"

#include <memory>

namespace audio { class AudioBackend; }

class twSpeaker
    : public twComponent
{
    Q_OBJECT
private:
    std::unique_ptr<audio::AudioBackend> backend_;
    bool isPlaying_;

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

public slots:
    void startOutput();
    void stopOutput();
};

#endif
