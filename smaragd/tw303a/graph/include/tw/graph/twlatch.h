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
    // Non-owning back-reference to the component that owns this latch. The
    // owner holds the latch strongly (twComponent::pOutputLatches_), so a
    // shared_ptr here would form a component<->latch cycle that leaks the
    // whole DSP subgraph. weak_ptr breaks the cycle; getComponent() locks it.
    std::weak_ptr<twComponent> component;
    idx_t idx;
protected:
    // The latch owns its outputs. Consumers hold a shared_ptr to the plug they
    // read from (twComponent::pInputPlugs_), so a plug snapshotted by the audio
    // thread stays alive across a concurrent disconnect. deleteOutput() drops
    // this list's reference; the object frees once the last consumer releases it.
    std::vector<std::shared_ptr<twLatchOutput>> outputList;
    // the current top offset of the Latch
    offset_t offset;

public:
    twLatch( std::shared_ptr<twComponent> component0, idx_t idx0 );

    virtual offset_t getOffset () { return offset; }
    virtual void resetOffset() { offset = 0; }  // Reset for capture rebuilds
    virtual ~ twLatch();

    inline std::shared_ptr<twComponent> getComponent() { return component.lock(); }
    inline idx_t getIndex() { return idx; }

    // Native format of the data this latch produces. The default reports the
    // canonical mono-Float32 format at the environment sample rate, so every
    // existing latch tells the truth without any change. Producers that emit a
    // different format (foreign rate, Int16 PCM, …) override this.
    virtual twFormat getFormat() const;

    virtual twLatchOutput * addOutput();
    virtual int deleteOutput( twLatchOutput * latchOutput );

    // Return the owning shared_ptr for a raw output pointer this latch created,
    // so a consumer can share ownership of the plug it reads from. Returns
    // nullptr if the output does not belong to this latch.
    std::shared_ptr<twLatchOutput> sharedOutput( twLatchOutput * latchOutput );

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

    int sampleRate_;                                   // Project sample rate for freezePage()

public:
    twStreamingLatch( std::shared_ptr<twComponent> comp, idx_t idx0, length_t bufSize0 );
    virtual ~ twStreamingLatch ();

    // Streaming latches hand out streaming outputs. The base twLatch::addOutput
    // allocates a plain twLatchOutput; override so the object is really a
    // twLatchStreamingOutput. Consumers static_cast their input plug to that
    // type, and it now carries per-reader state (its own page-chain hint), so
    // allocating the exact type is REQUIRED for correctness, not just tidiness.
    virtual twLatchOutput * addOutput() override;

    // Serve up to maxLength frames from startOffset (a timeline position),
    // sourced from position-aligned frozen pages of the producing component.
    //
    // readerPrevPage is the CALLING reader's OWN page-chain hint: the frozen
    // page it last served, used to continue stateful DSP (reverb/filter memory)
    // across page boundaries. It lives on the reader (twLatchStreamingOutput),
    // not on the shared latch, so fan-out readers at different positions cannot
    // clobber one another. It is read/written here only via std::atomic_load/
    // std::atomic_store, because a double-render of one reader can enter this
    // concurrently on two freeze threads — the hint is advisory (a wrong value
    // costs at most a reset+seek discontinuity), so last-writer-wins is fine,
    // but the shared_ptr access itself must be atomic to avoid a refcount race.
    length_t copyData( offset_t startOffset, sample_t *pDest, length_t maxLength,
                       std::shared_ptr<twOutputPage>& readerPrevPage );

    static const int bufSizeDefault;
};

class twLatchStreamingOutput
    : public twLatchOutput
{
private:
    // This reader's private page-chain hint: the frozen page it most recently
    // served. Handed to twStreamingLatch::copyData so DSP state chains across
    // page boundaries for THIS reader, independently of any other reader that
    // shares the same latch. Accessed exclusively via std::atomic_load/store
    // (see copyData) because the same reader can be entered by two freeze
    // threads during a double-render. shared_ptr so the page survives a
    // concurrent page invalidation while we still reference it.
    std::shared_ptr<twOutputPage> previousPage_;
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
