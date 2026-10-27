#include "app/objects/sampler/ssamplerprogram.h"

#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/persistence/sprojectloader.h"

#include "tw/plugins/twplugin.h"
#include "tw/sampler/twsamplerhost.h"
#include "tw/sampler/twsamplertypes.h"

#include <QDebug>
#include <QDomElement>
#include <QTextStream>

#include <algorithm>
#include <string>

SSamplerProgram::SSamplerProgram( SProject *project ) : SObject( project )
{
    setSName( QStringLiteral( "NassauDigital program" ) );
}

SSamplerProgram::~SSamplerProgram() = default;

// --- the program ---------------------------------------------------------------

bool SSamplerProgram::edit( const std::function<bool( audio::twSamplerProgramModel & )> &fn,
                            QString *why )
{
    if( !isReadable() ) {
        if( why )
            *why = QStringLiteral( "the stored program could not be read (%1); it is kept "
                                   "verbatim and is not editable" ).arg( loadError_ );
        return false;
    }
    // A COPY, so a refusal anywhere inside `fn` leaves the program exactly as
    // it was even if `fn` made more than one core call before refusing.
    audio::twSamplerProgramModel work = model_;
    if( !fn( work ) ) return false;
    model_ = std::move( work );
    publish_();
    emit programChanged();
    return true;
}

// --- the selection ---------------------------------------------------------------

QVector<std::uint32_t> SSamplerProgram::selectedZones() const
{
    QVector<std::uint32_t> out;
    for( std::uint32_t id : selection_ )
        if( model_.hasZone( id ) ) out.append( id );
    return out;
}

void SSamplerProgram::setSelection( const QVector<std::uint32_t> &ids )
{
    if( ids == selection_ ) return;
    selection_ = ids;
    emit selectionChanged();
}

// --- the hand-over ---------------------------------------------------------------

SSamplerProgram::InstanceInitializer SSamplerProgram::instanceInitializer() const
{
    // The snapshot is captured BY VALUE: the initializer the slot keeps for its
    // next rebuild installs exactly the program that was current when it was
    // handed over, and a later edit hands over a new one.
    std::shared_ptr<const audio::twSamplerProgram> snap = snapshot_;
    return [snap]( audio::twPlugin &p ) {
        if( audio::twSamplerHost *h = dynamic_cast<audio::twSamplerHost *>( &p ) )
            h->setProgram( snap );
    };
}

void SSamplerProgram::setInstaller( Installer installer )
{
    installer_ = std::move( installer );
    if( installer_ ) publish_();
}

void SSamplerProgram::publish_()
{
    // An unreadable program plays the EMPTY program (the default model), never
    // a stale one.
    snapshot_        = model_.snapshot();
    publishedSerial_ = snapshot_ ? snapshot_->serial() : 0;
    if( installer_ ) installer_( instanceInitializer() );
}

// --- persistence -------------------------------------------------------------------

namespace {

// The core's text inside a CDATA section. The core escapes '>' wherever it
// writes text, so "]]>" cannot occur in what it writes; a verbatim kept text
// could in principle carry one, and splitting the section is the standard way
// to keep it intact.
QString cdata( const QString &text )
{
    QString body = text;
    body.replace( QStringLiteral( "]]>" ), QStringLiteral( "]]]]><![CDATA[>" ) );
    return QStringLiteral( "<![CDATA[" ) + body + QStringLiteral( "]]>" );
}

}  // namespace

int SSamplerProgram::serialize( QTextStream &o )
{
    const char *cls = metaObject()->className();
    o << "<" << cls;
    const int res = serializeSelfAttributes( o );
    if( res < 0 ) return res;
    o << ">\n";
    const QString text = isReadable() ? QString::fromStdString( model_.toXml() ) : rawText_;
    o << "<program>" << cdata( text ) << "</program>\n";
    o << "</" << cls << ">\n";
    return 0;
}

SLink *SSamplerProgram::instantiateFromDomElement( SProjectLoader &loader, QDomElement &element,
                                                   SObject *parent )
{
    (void) parent;
    SSamplerProgram *prog = new SSamplerProgram( &loader.getProject() );
    prog->readPreChildrenAttributes( element );

    const QDomElement pe = element.firstChildElement( QStringLiteral( "program" ) );
    const QString text   = pe.isNull() ? QString() : pe.text();
    std::string err;
    if( text.trimmed().isEmpty() ) {
        prog->loadError_ = QStringLiteral( "the element holds no program" );
    } else if( !audio::twSamplerProgramModel::fromXml( text.toStdString(), prog->model_, &err ) ) {
        prog->loadError_ = QString::fromStdString( err );
    }
    if( !prog->isReadable() ) {
        prog->rawText_ = text;
        qWarning().noquote()
            << QStringLiteral( "NassauDigital: the program stored in the project cannot be read "
                               "(%1); it is KEPT VERBATIM and written back on save, the "
                               "instrument plays nothing, and edits are refused" )
                   .arg( prog->loadError_ );
    }

    prog->readPostChildrenAttributes( element );
    return new SLink( *prog );
}

// Self-registration with the project loader (proposal 14, Phase 5), as
// SPluginSlot does: legal because the app is an OBJECT library.
static const bool s_registered_ssamplerprogram =
    ( SProjectLoader::registerSObjectClass( "SSamplerProgram",
                                            SSamplerProgram::instantiateFromDomElement ),
      true );
