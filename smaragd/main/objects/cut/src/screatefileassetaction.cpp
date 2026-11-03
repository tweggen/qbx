#include "app/objects/cut/screatefileassetaction.h"

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"
#include "tw/core/twfraction.h"

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>

namespace {

QString generateRecordingName( SProject *project )
{
    for( int n = 1;; ++n ) {
        const QString candidate = QStringLiteral( "Recording %1" ).arg( n );
        if( !project->hasAsset( candidate ) ) return candidate;
    }
}

SApplyResult refuse( const QString &why )
{
    qWarning().noquote() << QStringLiteral( "create-file-asset: REFUSED: %1" ).arg( why );
    return { false, nullptr };
}

}  // namespace

SCreateFileAssetAction::SCreateFileAssetAction( const QString &filePath, offset_t srcOffset,
                                                length_t length, const QString &assetName )
    : filePath_( filePath ), srcOffset_( srcOffset ), length_( length ), assetName_( assetName )
{
}

SApplyResult SCreateFileAssetAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    if( filePath_.isEmpty() ) return refuse( QStringLiteral( "no filePath" ) );
    if( srcOffset_ < 0 ) return refuse( QStringLiteral( "srcOffset is negative" ) );
    if( !assetName_.isEmpty() && project->hasAsset( assetName_ ) )
        return refuse( QStringLiteral( "the name \"%1\" is in use" ).arg( assetName_ ) );

    QString path = filePath_;
    SLink *probe = project->linkToFile( path );
    if( !probe ) return refuse( QStringLiteral( "\"%1\" does not open as audio" ).arg( filePath_ ) );
    SObject &file = probe->getSObject();
    const length_t fileLen = file.hasDuration() ? file.getDurationBlocking() : 0;
    if( fileLen <= 0 || srcOffset_ >= (offset_t)fileLen ) {
        delete probe;
        return refuse( QStringLiteral( "\"%1\" holds %2 frame(s); srcOffset %3 leaves nothing" )
                           .arg( filePath_ ).arg( (qlonglong)fileLen ).arg( (qlonglong)srcOffset_ ) );
    }
    length_t span = (length_t)( (offset_t)fileLen - srcOffset_ );
    if( length_ > 0 && length_ < span ) span = length_;

    const QString name = assetName_.isEmpty() ? generateRecordingName( project ) : assetName_;
    SCut *cut = new SCut( project, file );
    cut->setWindow( Fraction( (int64_t)srcOffset_ ), ClipLen( span ), WarpedLen( 0 ), Fraction( 1 ) );
    cut->setSName( name );
    // The cut holds its own content link; the probe was a planning temp, and a
    // leftover unparented link would pin the file forever (place-recording's
    // reason too).
    delete probe;
    project->registerAsset( name, cut );
    // The redo registers the SAME name.
    assetName_ = name;

    qInfo().noquote() << QStringLiteral( "create-file-asset: \"%1\" over %2 [%3, +%4)" )
                             .arg( name, filePath_ )
                             .arg( (qlonglong)srcOffset_ ).arg( (qlonglong)span );

    SAction *inv = SActionRegistry::instance().create( QStringLiteral( "remove-asset" ) );
    if( inv ) {
        QDomDocument doc;
        QDomElement  el = doc.createElement( QStringLiteral( "remove-asset" ) );
        el.setAttribute( QStringLiteral( "assetName" ), name );
        doc.appendChild( el );
        inv->setPathRoot( pathRoot() );
        inv->readXml( el, inv->formatVersion() );
    }
    return { true, inv };
}

void SCreateFileAssetAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "srcOffset", (qlonglong)srcOffset_ );
    elem.setAttribute( "length", (qlonglong)length_ );
    if( !assetName_.isEmpty() ) elem.setAttribute( "assetName", assetName_ );
}

bool SCreateFileAssetAction::readXml( const QDomElement &elem, int /*version*/ )
{
    filePath_  = elem.attribute( "filePath" );
    srcOffset_ = (offset_t)elem.attribute( "srcOffset", "0" ).toLongLong();
    length_    = (length_t)elem.attribute( "length", "0" ).toLongLong();
    assetName_ = elem.attribute( "assetName" );
    return true;
}

QStringList SCreateFileAssetAction::knownAttributes() const
{
    return { QStringLiteral( "filePath" ), QStringLiteral( "srcOffset" ),
             QStringLiteral( "length" ), QStringLiteral( "assetName" ) };
}

static const bool s_reg_createfileasset = (
    SActionRegistry::instance().registerType( QStringLiteral( "create-file-asset" ),
                                              [] { return new SCreateFileAssetAction; } ),
    true );
