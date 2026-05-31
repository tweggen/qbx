
#include <qobject.h>
#include <QDebug>

#include <cstdint>

using namespace std;

#include "sobject.h"
#include "sproject.h"
#include "sstdmixer.h"
#include "sexternfile.h"

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
    return 0;
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
    DTOR_DEL( soRoot_ );
}

SProject::SProject()
    : soRoot_( NULL ),
      bpmTempo_( 120. ),
      sampleRate_( 48000 ),
      candidateRates_{ 44100, 48000, 88200, 96000 }
{
#if 0
    soRoot_ = new SStdMixer( this );
#endif
}
