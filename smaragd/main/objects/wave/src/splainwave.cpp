
#include <stdio.h>

#include <qobject.h>
#include <qwidget.h>
#include <qlabel.h>
#include "qmessagebox.h"

#include "app/model/sappcontext.h"
#include "app/model/sproject.h"
#include "tw/schedule/capture_aspects.h"  // Preview/Playback/... bits
#include "tw/graph/twcomponent.h"
#include "tw/sources/twwavinput.h"
#include "app/objects/wave/splainwave.h"
#include "app/objects/wave/splainwaverndrinline.h"
#include "app/persistence/sprojectloader.h"

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
    // Phase 5e: Use page cache for preview (async revalidation model)
    // Try to get cached preview first, fall back to live preview if unavailable

    if (!cpWave_) return -1;

    // Try page cache first (may be stale or invalid, which is acceptable)
    auto page = getCapture(Preview);
    if (page) {
        std::lock_guard<std::mutex> pageLock(page->pageMutex);
        if (page->validAspects & Preview) {
            // Cache has valid preview; copy from page into dest
            const preview_t* pagePreview = reinterpret_cast<const preview_t*>(page->data);
            const offset_t previewCount = CapturePageData::PAGE_SIZE / sizeof(preview_t);

            // Calculate how much preview data to copy into dest
            offset_t copyCount = std::min((offset_t)nProbes, previewCount);
            memcpy(dest, pagePreview, copyCount * sizeof(preview_t));
            return copyCount;
        }
    }

    // Cache not ready; fall back to live preview
    return getStraightPreview(dest, start, length, nProbes);
}


QString SPlainWave::getFileName() const
{
    return fileName_;
}

twComponent &SPlainWave::getRootComponent()
{
    return *cpWave_;
}

twRandomSource *SPlainWave::getRandomSource()
{
    return cpWave_ ? cpWave_->getSource() : NULL;
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
    cpWave_ = new twWavInput( *(SAppContext::get().get303aEnvironment()), fileName );
    cpWave_->init();
    if( !cpWave_->wasLoaded() ) {
        // Suppress dialog in headless/test mode; log to stderr instead
        if( SAppContext::get().testOutputDir().isEmpty() ) {
            QMessageBox::information( nullptr, "QBX error", "Unable to load file.", QMessageBox::Ok );
        } else {
            qWarning() << "SPlainWave: unable to load file:" << fileName;
        }
        delete cpWave_;
        cpWave_ = NULL;
        return -1;
    }
    // Add myselves tob the list of extern objects.
    qWarning() << "Filename here is" << fileName_;
    SAppContext::get().getCurrentProject()->addExternObject( *this );
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

// Phase 5e: Page cache implementation

// Self-registration with the project loader (proposal 14, Phase 5): the
// persistence module names no concrete types; each slice registers its own
// element name. Relies on the app being an OBJECT library (no TU elision).
static const bool s_registered_splainwave =
    ( SProjectLoader::registerSObjectClass( "SPlainWave",
          SPlainWave::instantiateFromDomElement ), true );

// Extern-file factory for the model (proposal 14, Phase 5): SProject names
// no concrete types; the wave slice provides the WAV-file loader.
static SExternFile *createPlainWaveFile( SProject *project, QString &fileName )
{
    SPlainWave *w = new SPlainWave( project );
    if( w->setWave( fileName ) < 0 ) {
        delete w;
        return nullptr;
    }
    return w;
}
static const bool s_registered_wavefactory =
    ( SProject::registerExternFileFactory( createPlainWaveFile ), true );
