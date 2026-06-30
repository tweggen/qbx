
#ifndef _TW_TRACKMIX_H
#define _TW_TRACKMIX_H

#include <qobject.h>
#include <atomic>
#include <vector>

#include "twcomponent.h"

class tw303aEnvironment;

// Clip entry: timeline position and DSP component
struct ClipEntry {
    offset_t     startTime;
    length_t     duration;      // 0 = unbounded
    twComponent *component;     // borrowed; lifetime managed by caller
};

class twTrackMix
    : public twComponent
{
    Q_OBJECT
public:
    twTrackMix( tw303aEnvironment &env );
    ~twTrackMix();

    // Clip management (called by STrack on the UI thread)
    // These are protected by the inherited mutex() from twComponent
    void insertClip(offset_t startTime, length_t duration, twComponent *component);
    void removeClip(twComponent *component);
    void updateClip(twComponent *component, offset_t newStartTime, length_t newDuration);

    // Track intrinsic properties (gain, mute) — applied to all output
    void setTrackMute(bool muted);
    void setTrackGain(double gainDb);

public slots:
    virtual void setBufferSize( length_t );

public:
    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );
    virtual void createOutputLatches();

    virtual idx_t getNInputs() const;
    virtual idx_t getNOutputs() const;
    virtual const char *getInputName( idx_t ) const;
    virtual const char *getOutputName( idx_t ) const;

    virtual length_t calcOutputTo( sample_t *, length_t, idx_t );

    // Phase 3: Page-based rendering — freeze track output to pages
    // Enables renderObjectInto replacement and unified page-based pipeline
    virtual std::shared_ptr<twOutputPage> freezePage(
        uint64_t startPos,
        const sample_t *inputData,
        uint64_t inputOffset,
        length_t inputLength,
        int sampleRate,
        std::shared_ptr<twOutputPage> previousPage = nullptr
    ) override;

protected:

    virtual void reset() override;

private:
    // Helpers: protect clips_ iteration against concurrent modification.
    // These use the inherited mutex() from twComponent base class to avoid
    // introducing a second mutex which could cause deadlock.
    int seekTo_nolock(offset_t newOffset);
    length_t calcOutputTo_nolock(sample_t *buffer, length_t playLen, idx_t outChannel);
    length_t freezePage_nolock(std::shared_ptr<twOutputPage> page, uint64_t startPos,
                               length_t length, int sampleRate,
                               std::shared_ptr<twOutputPage> previousPage);

    std::vector<ClipEntry> clips_;            // Timeline entries (sorted by startTime)
    std::atomic<offset_t> playOffset_{ 0 };   // Atomic: protects race between UI seek and audio render
    bool trackMuted_{ false };                 // Track mute state
    double trackGainDb_{ 0.0 };                // Track gain in dB

};

#endif
