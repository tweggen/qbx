
#ifndef _TWWAVINPUT_H
#define _TWWAVINPUT_H

#include <memory>

#include "tw/core/twcontenthash.h"
#include "tw/graph/twcomponent.h"

class tw303aEnvironment;
class twSampleSource;
class twRandomSource;

/**
 * Audio input component that plays a WAV file.
 *
 * Since proposal 07 this is a thin cursor: it owns a fully-resident
 * twSampleSource (the immutable, shared sample data) and serves calcOutputTo()
 * by random-reading that source at its current play position. The file is read
 * once, at load time, into RAM — there is no QFile handle, no mutex, and no
 * file I/O in the realtime path, so the old UI/audio race is gone.
 *
 * Consumers that want INDEPENDENT play positions (e.g. several SCuts of one
 * sample) should acquire their own twSampleReader from getSource() rather than
 * share this single cursor; getSource() exposes the underlying data for that.
 */
class twWavInput
    : public twComponent
{
public:
    twWavInput( tw303aEnvironment &env, QString fileName );
    virtual ~twWavInput();

    virtual void createOutputLatches() override;

    virtual int setNOutputs( idx_t );
    virtual length_t getLength() const;
    virtual length_t setCacheSize( length_t );
    virtual length_t getCacheSize() const;

    virtual bool isSeekable() const override;
    virtual int seekTo( offset_t ) override;

    virtual const char *getInputName( idx_t ) const override;
    virtual const char *getOutputName( idx_t ) const override;
    virtual idx_t getNInputs() const override;
    virtual idx_t getNOutputs() const override;
    virtual void setBufferSize( length_t ) override;
    virtual void init() override;

    // Phase 3: IOVector-based interface (type-safe, page-backed)
    virtual length_t calcOutputTo( IOVector& dest, idx_t idx ) override;

    bool wasLoaded() const { return loaded_; }

    // The immutable, shared sample data behind this input. Consumers use this to
    // mint independent readers (twRandomSource::acquireReader).
    twRandomSource *getSource() const;

    // Content digest of the decoded sample data (proposal 27 sidecar key);
    // null when nothing is loaded.
    twContentHash contentHash() const;

    // The SOURCE-RATE resident sample data (proposal 27 M1: analysis jobs
    // must read native-rate PCM so results are stable across project rates).
    // Owned by this input; callers needing lifetime beyond it hold the
    // twWavInput shared_ptr itself. Null when nothing is loaded.
    twSampleSource *sampleSource() const { return loaded_ ? source_.get() : nullptr; }

    virtual void reset() override;

    // Single file cursor (pos_): freezes must be serialized (proposal 19 Ph1).
    bool usesSerialCursor() const override { return true; }

    // Teardown protocol
    virtual void teardown() override;

private:
    // Helper: do seek work outside lock (caller must hold mutex)
    int seekTo_nolock(offset_t newOffset);

    // Helper: do reset work outside lock (caller must hold mutex)
    void reset_nolock();

    // shared_ptr since proposal 27 M5: the streaming grain path
    // co-owns the sample data via twRandomSource::sharedRef().
    std::shared_ptr<twSampleSource> source_;
    bool            loaded_;
    offset_t        playOffset_;
    QString         fileName_;
};

#endif
