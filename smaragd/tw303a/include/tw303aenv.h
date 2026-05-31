
#ifndef _TW303AENV_
#define _TW303AENV_

#include <qobject.h>
#include <qlist.h>

#include <cstdint>
#include <vector>

#include "twcomponent.h"

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
};

#endif
