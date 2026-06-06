
#include <stdio.h>

#include <qobject.h>
#include <qwidget.h>
#include <qlabel.h>
#include "qmessagebox.h"

#include "sapplication.h"
#include "sproject.h"
#include "twcomponent.h"
#include "twwavinput.h"
#include "splainwave.h"
#include "splainwaverndrinline.h"
#include "sprojectloader.h"

int SPlainWave::serializeSelfAttributes( QTextStream &o )
{
    o << " filename='" << getFileName() << "'";
    SExternFile::serializeSelfAttributes( o );
    return 0;
}

bool SPlainWave::hasPreview() const
{
    return true;
}

int SPlainWave::getPreview( preview_t *dest,
                             offset_t start, length_t length,
                             offset_t nProbes )
{
    // Only create preview, if have the wave loaded.
    if( !cpWave_ ) return -1;
    return getStraightPreview( dest, start, length, nProbes );
}

QString SPlainWave::getFileName() const
{
    return fileName_;
}

twComponent &SPlainWave::getRootComponent()
{
    return *cpWave_;
}

QWidget *SPlainWave::getDetailEditWidget( QWidget *parent )
{
    // FIXME: Reset pointer on destroy.
    return new QLabel( "plainWave: Nothing to edit now.", parent );
}

QWidget *SPlainWave::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *SPlainWave::getInlineRenderer()
{
    if( !inlineRenderer_ ) {
        inlineRenderer_ = new SPlainWaveRendererInline( *this );
    }
    return inlineRenderer_;
}

bool SPlainWave::hasDuration() const
{
    return cpWave_ != NULL;
}

length_t SPlainWave::getDuration() const
{
    if( cpWave_ ) {
        return cpWave_->getLength();
    } else {
        return 0;
    }
}

int SPlainWave::setWave( const QString fileName )
{
    // Fail, if we already have a wave set.
    if( cpWave_ ) return -2;
    fileName_ = fileName;
    cpWave_ = new twWavInput( *(SApplication::app().get303aEnvironment()), fileName );
    cpWave_->init();
    if( !cpWave_->wasLoaded() ) {
        QMessageBox::information( nullptr, "QBX error", "Unable to load file.", QMessageBox::Ok );
        delete cpWave_;
        cpWave_ = NULL;
        return -1;
    }
    // Add myselves tob the list of extern objects.
    qWarning() << "Filename here is" << fileName_;
    SApplication::app().getCurrentProject()->addExternObject( *this );
    return 0;
}

SPlainWave::~SPlainWave()
{
    if( cpWave_ ) {
        // Deregister from our OWN project (our QObject parent). Using the app's
        // "current" project was wrong: it is NULL during File -> Close and points
        // at the wrong project when loading into a non-current project. SProject's
        // destructor deletes its children before tearing down externFileDict_, so
        // the dict is still alive here.
        if( QObject *p = parent() ) {
            static_cast<SProject*>( p )->removeExternObject( fileName_ );
        }
    }
}

SPlainWave::SPlainWave( SProject *project ) 
    : SExternFile( project ),
      cpWave_( NULL ),
      fileName_( "" ),
      inlineRenderer_( NULL )
{    
}

SLink *SPlainWave::instantiateFromDomElement( 
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    (void) parent;
    // Ignore other parameters.
    //
    // NB: use the QString directly. Casting QString::data() (QChar*, UTF-16) to
    // const char* truncates the path at the first byte (e.g. "C:/..." -> "C").
    QString fileName = element.attribute( "filename" );
    if( fileName.isEmpty() ) {
        qWarning() << "SPlainWave: missing/empty filename attribute.";
        return NULL;
    }
    return projectLoader.getProject().linkToFile( fileName );
}
