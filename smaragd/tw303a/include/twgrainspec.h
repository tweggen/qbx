
#ifndef _TW_GRAINSPEC_H_
#define _TW_GRAINSPEC_H_

#include <qobject.h>
#include "tw303aenv.h"

class twComponent;

class twSingleGrainSpec
{
public:
    // Start in source.
    offset_t startOffset_;
    length_t length_;
    short type_, res0_;
    int pitchOffset_;
    int res1_;
    // Start overlap time.
    offset_t overlapIn_;
    // End overlap time.
    offset_t overlapOut_;
};

/**
 * A twGrainSpec defines the distribution of grains in a given
 * sample. It does not define the overall playback position/speed
 * pitch or what you call it.
 */
class twGrainSpec 
    : public QObject 
{
    Q_OBJECT
public:    
    twGrainSpec( twComponent &, twSingleGrainSpec *, int nGrains );
    virtual ~twGrainSpec();    
    virtual void init();

    twSingleGrainSpec *getGrain( int idx );
    void setGrains( twSingleGrainSpec *, int nGrains );
    int getNGrains() const;
    twSingleGrainSpec *getGrainArray() const;    

    int getTotalLength() const
        { return totalLength_; }

    twComponent &getSource() const { return source_; }
    
protected:
private:
    int nGrains_;
    twSingleGrainSpec *grains_;
    twComponent &source_;
    length_t totalLength_;
};

#endif
