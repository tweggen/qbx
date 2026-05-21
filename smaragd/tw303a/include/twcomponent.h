
#ifndef _TWCOMPONENT_H_
#define _TWCOMPONENT_H_

#include <qobject.h>

#include "exc.h"

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
    virtual ~ twLatch();

    inline twComponent & getComponent() { return component; }
    inline idx_t getIndex() { return idx; }
    
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
    
    virtual void setBufferSize( length_t ) {};

    int initOperation( int );

signals:
    void inputChanged( idx_t idx, twLatchOutput *former, twLatchOutput *recent );
    void outputChanged( idx_t idx, twLatchOutput *former, twLatchOutput *recent );
};

#include "tw303aenv.h"

#endif

