
#ifndef _SPROJECT_H
#define _SPROJECT_H

#include <qobject.h>
#include <qstring.h>
//#include <qptrlist.h>
#include <qhash.h>
#include <qtextstream.h>
#include <QVariant>
#include <QVariantMap>

#include <cstdint>
#include <vector>

class QDomElement;

class SObject;
class SExternFile;
class SLink;

struct SObjectRegistryEntry;

class SProject 
    : public QObject
{
    Q_OBJECT
public:
    SProject();
    virtual ~SProject();

    const QString &getFileName();
    SObject *getRootComponent() const;
    void setRootComponent( SObject * );

    SLink *linkToFile( QString & );

    virtual int serialize( QTextStream & );
    double getBPMTempo() const { return bpmTempo_; }
    int serializeSelfAttributes( QTextStream & );
    int readPreChildrenAttributes( QDomElement &element );

    // Project sample rate (persisted; default 48000 for a fresh project, 44100
    // when loading a pre-sample-rate file). Applied to tw303aEnvironment when
    // the project becomes current.
    int getSRate() const { return sampleRate_; }
    const std::vector<std::uint32_t> &candidateRates() const { return candidateRates_; }

    // Generic per-project property store (a QVariantMap; serialized as JSON).
    // Named prop/setProp/hasProp rather than property/setProperty to avoid
    // shadowing QObject's meta-property API. Well-known keys + defaults live in
    // sprojectprops.h.
    QVariant prop( const QString &key, const QVariant &defaultValue = QVariant() ) const;
    void setProp( const QString &key, const QVariant &value );
    bool hasProp( const QString &key ) const;
    const QVariantMap &properties() const { return properties_; }


signals:
    void fileNameChanged( const QString & );
    void externFileAdded( const SExternFile & );
    void externFileRemoved( const QString );
    void bpmTempoChanged( double );
    void sampleRateChanged( int );
    void propertyChanged( const QString &key, const QVariant &value );

public slots:
    void setFileName( const QString & );
    void addExternObject( const SExternFile & );
    void removeExternObject( QString & );
    void setBPMTempo( double );
    void setSRate( int );
    void setCandidateRates( std::vector<std::uint32_t> );

protected:

private slots:

private:
    QString fileName_;
    SLink *soRoot_;
    double bpmTempo_;
    int sampleRate_;
    std::vector<std::uint32_t> candidateRates_;
    QVariantMap properties_;

    QHash<QString,SExternFile*> externFileDict_;
};

#endif
