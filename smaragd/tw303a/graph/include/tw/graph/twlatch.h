#ifndef _TWLATCH_H_
#define _TWLATCH_H_

// Latch machinery: the producer/consumer plumbing between components
// (proposal 14, Phase 1 — extracted from twcomponent.h, de-Qt'd:
// QList -> std::vector; this header must stay Qt-free).

#include <memory>
#include <vector>

#include "tw/core/twtypes.h"
#include "tw/core/exc.h"
#include "tw/core/twformat.h"
#include "tw/pages/tw_output_page.h"

class tw303aEnvironment;
class twComponent;
class twLatchOutput;

class twLatch
{
private:
    twLatch();
    std::shared_ptr<twComponent> component;
    idx_t idx;
protected:
    std::vector<twLatchOutput*> outputList;
    // the current top offset of the Latch
    offset_t offset;

public:
    twLatch( std::shared_ptr<twComponent> component0, idx_t idx0 );

    virtual offset_t getOffset () { return offset; }
    virtual void resetOffset() { offset = 0; }  // Reset for capture rebuilds
    virtual ~ twLatch();

    inline std::shared_ptr<twComponent> getComponent() { return component; }
    inline idx_t getIndex() { return idx; }

    // Native format of the data this latch produces. The default reports the
    // canonical mono-Float32 format at the environment sample rate, so every
    // existing latch tells the truth without any change. Producers that emit a
    // different format (foreign rate, Int16 PCM, …) override this.
    virtual twFormat getFormat() const;

    virtual twLatchOutput * addOutput();
    virtual int deleteOutput( twLatchOutput * latchOutput );

};

class twLatchOutput
{
private:
    twLatch & parentLatch;
protected:
    offset_t offset;
public:
    twLatchOutput (twLatch & latch)
        : parentLatch(latch) { offset = latch.getOffset(); }
    virtual ~twLatchOutput() = default;
    inline twLatch & getParentLatch () { return parentLatch; }

    // Re-position this reader on the producer's timeline. Must be called when
    // the consuming component seeks; readStreamingData() otherwise keeps
    // pulling content for the old position.
    void seekStream( offset_t pos ) { offset = pos; }

    // The consumer's single entry point for "what am I about to read?".
    // Delegates to the producing latch.
    twFormat getFormat() const { return parentLatch.getFormat(); }

    virtual length_t readData( sample_t *, length_t  ) {
        throw excStandard( "twLatchOutput(): Tried to read data from the Latch itselves." );
    };
};

class twStreamingLatch
    : public twLatch
{
private:
    void init( length_t bufSize0 );
protected:
    sample_t * pBuffer;
    // defaults to 16384
    length_t bufSize;
    // the current position (equivalent to offset in parent class)
    offset_t bufPos;

    // Phase 2: freezePage() state tracking
    offset_t currentPos_;                              // Current playback position for snapshots
    std::shared_ptr<twOutputPage> previousPage_;      // Cached page for state continuity
    int sampleRate_;                                   // Project sample rate for freezePage()

public:
    twStreamingLatch( std::shared_ptr<twComponent> comp, idx_t idx0, length_t bufSize0 );
    virtual ~ twStreamingLatch ();

    length_t copyData( offset_t startOffset, sample_t *pDest, length_t maxLength );

    // Phase 2: Reset cached page on seek (breaks state chain)
    void resetPageCache() { previousPage_ = nullptr; }

    static const int bufSizeDefault;
};

class twLatchStreamingOutput
    : public twLatchOutput
{
private:
protected:
public:
    twLatchStreamingOutput (twStreamingLatch & latch)
        : twLatchOutput((twLatch &) latch) {}

    length_t readStreamingData( sample_t * pDest, length_t maxLength );
    inline length_t readData( sample_t * pDest, length_t maxLength )
        { return readStreamingData( pDest, maxLength ); }

    // Read up to maxFrames frames of the latch's NATIVE format (getFormat())
    // into dest, with no conversion; dest must hold
    // maxFrames * getFormat().bytesPerFrame() bytes. Returns frames read. The
    // sink decides whether to convert (see twConvertFrames). Current latches
    // store canonical mono Float32, so this presently yields the same bytes as
    // readStreamingData — a producer storing another representation overrides it.
    length_t readRaw( void * dest, length_t maxFrames );

    inline twStreamingLatch & getParentStreamingLatch()
        { return (twStreamingLatch &) getParentLatch(); }
};

#endif
