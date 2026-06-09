
#ifndef _TWCOMPONENT_H_
#define _TWCOMPONENT_H_

#include <qobject.h>

#include "exc.h"
#include "twformat.h"

#define DTOR_DEL(x) {if((x)) {delete (x); (x) = NULL; }}

#undef DEBUG_COMPONENT

typedef signed long long length_t;
typedef signed short idx_t;
typedef float sample_t;
#define SAMPLE_NORM_MIN (-1.0)
#define SAMPLE_NORM_MAX (1.0)
typedef unsigned long long offset_t;

// The type used for preview datas.
typedef signed char previewPart_t;
typedef struct {
    previewPart_t min, max;
} preview_t;


class tw303aEnvironment;
class twLatchOutput;
class twComponent;

class twLatch
    : public QObject
{
    Q_OBJECT
private:
    twLatch();
    twComponent & component;
    idx_t idx;
protected:
    QList<twLatchOutput*> outputList;
    // the current top offset of the Latch
    offset_t offset;
    
public:
    twLatch( twComponent & component0, idx_t idx0 );

    virtual offset_t getOffset () { return offset; }
    virtual void resetOffset() { offset = 0; }  // Reset for capture rebuilds
    virtual ~ twLatch();

    inline twComponent & getComponent() { return component; }
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
    : public QObject
{
    Q_OBJECT
private:
    twLatch & parentLatch;
protected:
    offset_t offset;
public:
    twLatchOutput (twLatch & latch)
        : parentLatch(latch) { offset = latch.getOffset(); }
    inline twLatch & getParentLatch () { return parentLatch; }

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
public:
    twStreamingLatch( twComponent & comp, idx_t idx0, length_t bufSize0 );
    virtual ~ twStreamingLatch ();
    
    length_t copyData( offset_t startOffset, sample_t *pDest, length_t maxLength );
    
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

class twComponent
    : public QObject
{
    Q_OBJECT
private:
    int currentOperation_;
    
protected:
    virtual int doInitOperation( int );
    
    int inputsSet_;
    tw303aEnvironment &env;
    twLatch ** pOutputLatches;
    twLatchOutput ** pInputPlugs;
    
    friend class twLatch;
    friend class twStreamingLatch;
    
public:
    twComponent( tw303aEnvironment & env );
    virtual ~twComponent();
    
    virtual bool isSeekable() const;
    virtual int seekTo( offset_t );
    virtual offset_t tellPos() const;
    virtual void resetAllLatches();  // Reset all output latches to offset 0

    virtual length_t calcOutputTo( sample_t *pDest, length_t length, idx_t idx ) = 0;

    void setInput( idx_t idx, twLatchOutput * pLatchOutput );
    virtual twLatchOutput *getInputPlug( idx_t ) const;
    int getInputsSet() const { return inputsSet_; }
    virtual twLatchOutput *linkOutput( idx_t idx );
    
    virtual void allocPlugs();
    virtual void init();
    virtual void createOutputLatches() = 0;
    
    virtual idx_t getNInputs() const = 0;
    virtual idx_t getNOutputs() const = 0;
    virtual const char *getInputName( idx_t idx ) const = 0;
    virtual const char *getOutputName( idx_t idx ) const = 0;

    // --- Format negotiation (proposal 04 §3) -----------------------------
    // Seed capability domains for one port. Default: mono Float32 at any rate.
    virtual twFormatCaps getOutputCaps( idx_t idx ) const;
    virtual twFormatCaps getInputCaps ( idx_t idx ) const;

    // The node's in<->out coupling relation, iterated to a fixpoint by the
    // negotiator. It narrows the given port domains to mutual consistency and
    // MUST be monotone (remove candidates only) — that is what guarantees
    // termination. Returns true iff it narrowed anything. The default couples
    // every port to one common rate (a node that neither resamples nor
    // rate-mixes); a rate-decoupling node (resampler) overrides this to return
    // false. Contract: domains are concrete (the negotiator has expanded "any"
    // to the candidate set D before calling), so an empty domain means
    // infeasible, not "any".
    virtual bool narrowCaps( twPortDomains &ports ) const;

    // Commit a single chosen format per port after the negotiation fixpoint.
    // The node records them and does any heavy, node-specific setup (a
    // resampler would build its kernel here). Default: record the formats and
    // emit formatChanged for any output whose format changed. Returns false if
    // the committed format is unworkable for this node.
    virtual bool commitFormats( const twFormat *in,  idx_t nIn,
                                const twFormat *out, idx_t nOut );
    
    virtual void setBufferSize( length_t ) {};

    int initOperation( int );

signals:
    void inputChanged( idx_t idx, twLatchOutput *former, twLatchOutput *recent );
    void outputChanged( idx_t idx, twLatchOutput *former, twLatchOutput *recent );
    // A node's constraints changed (e.g. a fixed-rate source reloaded); the
    // negotiator should re-run before the next play. Reserved for cached
    // negotiation — today twSpeaker re-negotiates on every startOutput, so the
    // trigger is implicit.
    void renegotiationRequired( twComponent *origin );
    // Emitted by commitFormats when an output port's negotiated format changes.
    void formatChanged( idx_t idx, twFormat oldFmt, twFormat newFmt );

private:
    std::vector<twFormat> committedIn_;
    std::vector<twFormat> committedOut_;
};

#include "tw303aenv.h"

#endif

