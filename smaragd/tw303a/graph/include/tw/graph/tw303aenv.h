
#ifndef _TW303AENV_
#define _TW303AENV_

#include <qobject.h>
#include <qlist.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "tw/graph/twcomponent.h"

QString QuoteString( const QString & );
QString UnquoteString( const QString & );


class twComponent;

class tw303aEnvironment
    : public QObject
{
    Q_OBJECT
private:
    QList<twComponent*> listModules;

    length_t bufferSize;
    int sampleRate;
    // The standard "magnet" rates the negotiator builds its candidate domain D
    // from (proposal 04 §3a). Configurable and persisted with the project.
    std::vector<std::uint32_t> candidateRates_;
protected:
public:
    tw303aEnvironment();
    virtual ~tw303aEnvironment();

signals:
    void componentAdded( twComponent & );
    void componentRemoved( twComponent & );
    // Emitted when the project/engine sample rate changes. Phase 7 routes this
    // into a renegotiation pass.
    void sampleRateChanged( int oldRate, int newRate );
    // Emitted when the candidate-rate set changes (also a renegotiation trigger).
    void candidateRatesChanged();

public:
    void setBufferSize( length_t size ) { bufferSize=size; };
    length_t getBufferSize() const { return bufferSize; };
    void addModule( twComponent *mod );
    void removeModule( twComponent *mod );
    int getSRate() const { return sampleRate; }
    void setSRate( int rate );

    const std::vector<std::uint32_t> &candidateRates() const { return candidateRates_; }
    void setCandidateRates( std::vector<std::uint32_t> rates );

    // Global content epoch: monotonically increasing counter bumped by any edit
    // that changes what the graph sounds like (clip insert/remove/move/resize,
    // track mute/gain, graph rewiring). Frozen output pages are stamped with the
    // epoch they were rendered at; consumers treat pages from an older epoch as
    // stale. Lock-free — safe to read from the audio thread.
    std::uint64_t contentEpoch() const { return contentEpoch_.load(std::memory_order_acquire); }
    void bumpContentEpoch() { contentEpoch_.fetch_add(1, std::memory_order_acq_rel); }

private:
    std::atomic<std::uint64_t> contentEpoch_{1};
};

#endif
