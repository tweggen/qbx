
#include <qobject.h>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdint>

using namespace std;

#include "sobject.h"
#include "sproject.h"
#include "sprojectprops.h"
#include "sstdmixer.h"
#include "sexternfile.h"

// Minimal XML escaping for embedding a JSON string inside a single-quoted
// attribute. JSON's own double quotes are fine inside single quotes; we only
// need to guard the characters that would break the attribute or the markup.
static QString xmlAttrEscape( const QString &s )
{
    QString out = s;
    out.replace( '&', "&amp;" );
    out.replace( '<', "&lt;" );
    out.replace( '\'', "&apos;" );
    return out;
}

#include "slink.h"

#include "splainwave.h"

QString QuoteString( const QString &org )
{
    return org;
}

QString UnquoteString( const QString &org )
{
    return org;
}

int SProject::serialize( QTextStream &o )
{
    o << "<SProject fileName='"+fileName_+"' rootId='";
    // As the link to he current root component is not my child, I
    // have to serialize it explicitely.
    if( soRoot_ ) {
        o << reinterpret_cast<std::uintptr_t>(&(soRoot_->getSObject()));
    } else {
        o << "0";
    }
    o << "'";
    serializeSelfAttributes( o );
    o << ">\n";
    const QObjectList& children = this->children();
    for (QObjectList::const_iterator it = children.begin(); it != children.end(); ++it ) {
        SObject* so = (SObject*) *it;        
        int res = so->serialize( o );
        if( res<0 ) break;
    }
    o << "</SProject>";
    return 0;
}

int SProject::serializeSelfAttributes( QTextStream &o )
{
    o << " bpmTempo='" << (double) getBPMTempo() << "'";
    o << " sampleRate='" << sampleRate_ << "'";
    o << " candidateRates='";
    for( size_t i = 0; i < candidateRates_.size(); ++i ) {
        if( i ) o << ",";
        o << candidateRates_[i];
    }
    o << "'";

    // The generic property dict, serialized as JSON in a single attribute.
    QJsonObject obj = QJsonObject::fromVariantMap( properties_ );
    QByteArray json = QJsonDocument( obj ).toJson( QJsonDocument::Compact );
    o << " properties='" << xmlAttrEscape( QString::fromUtf8( json ) ) << "'";
    return 0;
}

int SProject::readPreChildrenAttributes( QDomElement &element )
{
    QString data;
    data = element.attribute( "bpmTempo", "120.0" );
    setBPMTempo( data.toDouble() );

    // Absent on pre-sample-rate files → default 44100 so they load unchanged.
    setSRate( element.attribute( "sampleRate", "44100" ).toInt() );

    QString cr = element.attribute( "candidateRates", "44100,48000,88200,96000" );
    std::vector<std::uint32_t> rates;
    const QStringList parts = cr.split( ',', Qt::SkipEmptyParts );
    for( const QString &p : parts ) {
        bool ok = false;
        unsigned int v = p.trimmed().toUInt( &ok );
        if( ok && v > 0 ) rates.push_back( v );
    }
    if( !rates.empty() ) setCandidateRates( std::move( rates ) );

    // Generic property dict (JSON). Merge over the seeded defaults so missing
    // keys keep their default and unknown (future) keys are preserved.
    const QString pj = element.attribute( "properties" );
    if( !pj.isEmpty() ) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson( pj.toUtf8(), &err );
        if( err.error == QJsonParseError::NoError && doc.isObject() ) {
            const QVariantMap loaded = doc.object().toVariantMap();
            for( auto it = loaded.cbegin(); it != loaded.cend(); ++it ) {
                properties_.insert( it.key(), it.value() );
            }
        } else {
            qWarning() << "SProject: ignoring malformed properties JSON:" << err.errorString();
        }
    }
    return 0;
}

QVariant SProject::prop( const QString &key, const QVariant &defaultValue ) const
{
    return properties_.value( key, defaultValue );
}

void SProject::setProp( const QString &key, const QVariant &value )
{
    if( properties_.value( key ) == value ) return;   // no-op if unchanged
    properties_.insert( key, value );
    emit propertyChanged( key, value );
}

bool SProject::hasProp( const QString &key ) const
{
    return properties_.contains( key );
}

SObject *SProject::getRootComponent() const
{
    if( soRoot_ ) {
        return &(soRoot_->getSObject());
    } else {
        return NULL;
    }
}

void SProject::setRootComponent( SObject *obj )
{
    // No change?
    if( (soRoot_ && (obj==&(soRoot_->getSObject())))
        || (!soRoot_ && obj==NULL) ) return;
    // FIXME: Trigger views.
    if( soRoot_ ) {
        // Dereference old one.
        delete soRoot_;
        soRoot_ = NULL;
    }
    if( obj ) {
        soRoot_ = new SLink( *obj, NULL );
    }
}

void SProject::setBPMTempo( double newTempo )
{
    bpmTempo_ = newTempo;
    emit bpmTempoChanged( newTempo );
}

void SProject::setSRate( int rate )
{
    if( rate <= 0 ) return;
    sampleRate_ = rate;
    emit sampleRateChanged( rate );
}

void SProject::setCandidateRates( std::vector<std::uint32_t> rates )
{
    if( rates.empty() ) return;
    candidateRates_ = std::move( rates );
}

double SProject::getDurationSeconds() const
{
    // TODO: Calculate from arrangement/track structure
    // For now, return 60 seconds as a default
    return 60.0;
}

bool SProject::hasTimeSelection() const
{
    return prop(SProjectProps::RangeValid, false).toBool();
}

SProject::TimeRange SProject::getTimeSelection() const
{
    // Read from the UI's time-range marker (stored in samples)
    bool rangeValid = prop(SProjectProps::RangeValid, false).toBool();
    if (rangeValid) {
        // Convert from samples to seconds using project sample rate
        double sampleRate = getSRate();
        double startSec = prop(SProjectProps::RangeStart, 0).toULongLong() / sampleRate;
        double endSec = prop(SProjectProps::RangeEnd, 0).toULongLong() / sampleRate;
        return { startSec, endSec };
    }
    return { 0.0, getDurationSeconds() };
}

void SProject::setTimeSelection(double startSeconds, double endSeconds)
{
    // Store in the UI's time-range marker (convert from seconds to samples)
    double sampleRate = getSRate();
    setProp(SProjectProps::RangeValid, true);
    setProp(SProjectProps::RangeStart, (qulonglong)(startSeconds * sampleRate));
    setProp(SProjectProps::RangeEnd, (qulonglong)(endSeconds * sampleRate));
}

void SProject::clearTimeSelection()
{
    properties_.remove("render/timeSelection_in");
    properties_.remove("render/timeSelection_out");
}

void SProject::setFileName( const QString &fileName )
{
    fileName_ = fileName;
    emit fileNameChanged( fileName_ );
}

void SProject::addExternObject( const SExternFile &extObject )
{
    QString externFileName = extObject.getFileName();
    qWarning() << QString( "Name of extern file is \"%1\"." ).arg( externFileName );
    externFileDict_.insert( externFileName, const_cast<SExternFile*>( &extObject ) );
    emit externFileAdded( extObject );
}

void SProject::removeExternObject( QString &fileName )
{
    externFileDict_.remove( fileName );
    emit externFileRemoved( fileName );
}

void SProject::registerAsset( const QString &name, SObject *body )
{
    if( !body || name.isEmpty() ) return;
    if( assetDict_.contains( name ) ) {
        qWarning() << QString( "SProject::registerAsset: name already in use: \"%1\"." )
                          .arg( name );
        return;
    }
    assetDict_.insert( name, body );
    body->addRef();                  // pin: the asset survives with zero placements
    emit assetAdded( name, *body );
}

void SProject::unregisterAsset( const QString &name )
{
    SObject *body = assetDict_.take( name );
    if( !body ) return;
    emit assetRemoved( name );       // let listeners drop their row before the body dies
    body->removeRef();               // may bring the refcount to 0 -> deleteLater
}

SObject *SProject::asset( const QString &name ) const
{
    return assetDict_.value( name );
}

bool SProject::hasAsset( const QString &name ) const
{
    return assetDict_.contains( name );
}

QList<QString> SProject::assetNames() const
{
    return assetDict_.keys();
}

SLink *SProject::linkToFile( QString &fileName )
{
    SExternFile *ef = externFileDict_.value( fileName );
    if( !ef ) {
        // FIXME: Replace that by kind of factory (in SApplication)
        SPlainWave *w = new SPlainWave( this );
        if( w->setWave( fileName ) < 0 ) {
            delete w;
            return NULL;
        } 
        ef = w;
    }
    return new SLink( *ef );
}

SProject::~SProject()
{
    DTOR_DEL( soRoot_ );   // drop the root reference (parent==NULL, not a child)

    // Tear down the object graph here, in the destructor body, while our members
    // (externFileDict_, ...) are still alive — child destructors call back into
    // the project (e.g. SPlainWave deregisters itself from externFileDict_).
    //
    // Every SObject is both a QObject child of the project AND reference-counted
    // via SLinks. Delete only objects whose reference count has reached zero,
    // repeatedly: deleting an object frees its child SLinks, which drop the
    // references they held, bringing the next layer to zero. Starting from the
    // root (whose reference we just dropped) this cascades root -> leaf, so no
    // SLink ever dereferences an already-freed object.
    bool progress = true;
    while( progress ) {
        progress = false;
        const QObjectList kids = children();   // copy: mutates as we delete
        for( QObject *kid : kids ) {
            SObject *so = static_cast<SObject*>( kid );
            if( so->getNReferences() == 0 ) {
                delete so;
                progress = true;
            }
        }
    }

    // Safety net for any survivors (reference cycles / links leaked elsewhere):
    // delete them outright so we don't leak. Normally this list is empty.
    const QObjectList survivors = children();
    for( QObject *kid : survivors ) {
        delete kid;
    }
}

SProject::SProject()
    : soRoot_( NULL ),
      bpmTempo_( 120. ),
      sampleRate_( 48000 ),
      candidateRates_{ 44100, 48000, 88200, 96000 },
      properties_( SProjectProps::defaults() )
{
#if 0
    soRoot_ = new SStdMixer( this );
#endif
}
