#include "app/objects/sampler/ssampleractions.h"

#include "app/actions/sactionregistry.h"
#include "app/objects/sampler/ssamplerprogram.h"
#include "app/objects/sampler/ssamplerslots.h"

#include "tw/sampler/twsamplerprogrammodel.h"

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNamedNodeMap>
#include <QLocale>

#include <algorithm>
#include <vector>

using audio::twSamplerAttrs;
using audio::twSamplerEdit;
using audio::twSamplerParamKind;
using audio::twSamplerPrior;
using audio::twSamplerProgramModel;
using audio::twSamplerScope;

namespace {

// Attributes the .qxa runner and the registry own (sactionscript.cpp); never a
// program parameter.
bool isMetaAttribute( const QString &name )
{
    return name == QLatin1String( "expectReject" ) || name == QLatin1String( "version" ) ||
           name == QLatin1String( "nestedRejects" );
}

QStringList names( twSamplerScope scope )
{
    QStringList out;
    for( const std::string &n : twSamplerProgramModel::paramNames( scope ) )
        out << QString::fromStdString( n );
    return out;
}

twSamplerAttrs toCore( const SSamplerAttrList &attrs )
{
    twSamplerAttrs out;
    for( const auto &a : attrs ) out.emplace_back( a.first.toStdString(), a.second.toStdString() );
    return out;
}

// Every attribute of `elem` except the address, the runner's meta attributes
// and the names in `skip`, in document order.
SSamplerAttrList valueAttributes( const QDomElement &elem, const QStringList &skip )
{
    SSamplerAttrList out;
    const QDomNamedNodeMap attrs = elem.attributes();
    for( int i = 0; i < attrs.length(); ++i ) {
        const QDomAttr a = attrs.item( i ).toAttr();
        const QString  n = a.name();
        if( n == QLatin1String( "trackPath" ) || n == QLatin1String( "slotIndex" ) ) continue;
        if( isMetaAttribute( n ) || skip.contains( n ) ) continue;
        out.append( { n, a.value() } );
    }
    return out;
}

void writeAttributes( QDomElement &elem, const SSamplerAttrList &attrs )
{
    for( const auto &a : attrs ) elem.setAttribute( a.first, a.second );
}

// "17" -> {17}; "3;7;12" -> {3,7,12}. False on anything that is not a list of
// positive integers (an empty list included).
bool parseIdList( const QString &text, std::vector<std::uint32_t> &out )
{
    out.clear();
    const QStringList parts = text.split( QLatin1Char( ';' ), Qt::SkipEmptyParts );
    if( parts.isEmpty() ) return false;
    for( const QString &p : parts ) {
        bool ok = false;
        const qulonglong v = p.trimmed().toULongLong( &ok );
        if( !ok || v == 0 || v > 0xFFFFFFFFull ) return false;
        out.push_back( (std::uint32_t)v );
    }
    return true;
}

QString idListText( const QVector<std::uint32_t> &ids )
{
    QStringList parts;
    for( std::uint32_t id : ids ) parts << QString::number( id );
    return parts.join( QLatin1Char( ';' ) );
}

// The front end's value syntax (DESIGN §2.1; see ssampleractions.h). A text
// parameter is literal; otherwise `=` forces absolute, a leading sign makes a
// relative edit, and anything else is the absolute XML text.
bool toEdit( twSamplerScope scope, const QString &attr, const QString &text, twSamplerEdit &e,
             QString *why )
{
    e      = twSamplerEdit{};
    e.attr = attr.toStdString();
    const twSamplerParamKind kind = twSamplerProgramModel::paramKind( scope, e.attr );
    if( kind == twSamplerParamKind::String ) {
        e.value = text.toStdString();
        return true;
    }
    if( text.startsWith( QLatin1Char( '=' ) ) ) {
        e.value = text.mid( 1 ).toStdString();
        return true;
    }
    if( text.startsWith( QLatin1Char( '+' ) ) || text.startsWith( QLatin1Char( '-' ) ) ) {
        bool ok = false;
        const double d = QLocale::c().toDouble( text.startsWith( QLatin1Char( '+' ) )
                                                    ? text.mid( 1 ) : text, &ok );
        if( !ok ) {
            if( why )
                *why = QStringLiteral( "\"%1\" is not a relative number (a leading sign is a "
                                       "relative edit; write a negative absolute value as "
                                       "\"=%1\")" ).arg( text );
            return false;
        }
        e.relative = true;
        e.delta    = d;
        return true;
    }
    e.value = text.toStdString();
    return true;
}

twSamplerScope scopeOf( SSetSamplerValuesAction::Scope s )
{
    switch( s ) {
    case SSetSamplerValuesAction::Scope::Program: return twSamplerScope::Program;
    case SSetSamplerValuesAction::Scope::Zone:    return twSamplerScope::Zone;
    case SSetSamplerValuesAction::Scope::Layer:   return twSamplerScope::Layer;
    }
    return twSamplerScope::Zone;
}

bool scopeFromText( const QString &text, twSamplerScope &out )
{
    if( text == QLatin1String( "program" ) ) { out = twSamplerScope::Program; return true; }
    if( text == QLatin1String( "zone" ) )    { out = twSamplerScope::Zone;    return true; }
    if( text == QLatin1String( "layer" ) )   { out = twSamplerScope::Layer;   return true; }
    return false;
}

QString scopeText( twSamplerScope s )
{
    switch( s ) {
    case twSamplerScope::Program: return QStringLiteral( "program" );
    case twSamplerScope::Zone:    return QStringLiteral( "zone" );
    case twSamplerScope::Layer:   return QStringLiteral( "layer" );
    }
    return QStringLiteral( "zone" );
}

QList<SRestoreSamplerValuesAction::Prior> fromCore( const std::vector<twSamplerPrior> &in )
{
    QList<SRestoreSamplerValuesAction::Prior> out;
    for( const twSamplerPrior &p : in )
        out.append( { p.id, QString::fromStdString( p.attr ), QString::fromStdString( p.value ) } );
    return out;
}

void writeText( QDomElement &elem, const std::string &xml )
{
    // CDATA: the core's XML verbatim, so nothing between the remove and the
    // reinsert (QDom included, which sorts attributes) can reorder it.
    elem.appendChild( elem.ownerDocument().createCDATASection( QString::fromStdString( xml ) ) );
}

}  // namespace

// ============================================================ the address ===

SSamplerProgram *SSamplerAction::program( SProject *project, bool create, QString *why ) const
{
    return ssampler::programAt( project, pathRoot(), trackPath_, slotIndex_, create, why );
}

SApplyResult SSamplerAction::refuse( const QString &why ) const
{
    qWarning().noquote() << QStringLiteral( "%1: track %2 slot %3: REFUSED: %4" )
                                .arg( name(), trackPath_ ).arg( slotIndex_ ).arg( why );
    return { false, nullptr };
}

void SSamplerAction::writeAddress( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", slotIndex_ );
}

void SSamplerAction::readAddress( const QDomElement &elem )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
}

QStringList SSamplerAction::addressAttributes()
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ) };
}

// ============================================================== add-zone ====

SApplyResult SAddZoneAction::apply( SProject *project )
{
    QString why;

    // THE REDO: put back the zone the first apply made, ids and all.
    if( createdId_ != 0 ) {
        SSamplerProgram *prog = program( project, false, &why );
        if( !prog ) return refuse( why );
        std::string err;
        if( !prog->edit( [&]( twSamplerProgramModel &m ) {
                return m.reinsertZone( createdXml_, createdIndex_, &err );
            }, &why ) )
            return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
        SAction *inv = new SRemoveZoneAction( trackPath_, slotIndex_, createdId_ );
        return { true, inv };
    }

    // Validate on a scratch copy first, so the program OBJECT is created only
    // for an add that is going to succeed (a refusal mutates nothing).
    const twSamplerAttrs zoneAttrs = toCore( zone_ );
    std::vector<twSamplerAttrs> layerAttrs;
    for( const SSamplerAttrList &l : layers_ ) layerAttrs.push_back( toCore( l ) );

    SSamplerProgram *prog = program( project, false, nullptr );
    {
        twSamplerProgramModel scratch = prog ? prog->model() : twSamplerProgramModel();
        std::string err;
        if( scratch.addZone( zoneAttrs, layerAttrs, &err ) == 0 )
            return refuse( QString::fromStdString( err ) );
    }
    if( !prog ) prog = program( project, true, &why );
    if( !prog ) return refuse( why );

    std::uint32_t id = 0;
    std::string   err;
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            id = m.addZone( zoneAttrs, layerAttrs, &err );
            return id != 0 && m.zoneXml( id, &createdXml_, &createdIndex_ );
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
    createdId_ = id;

    qInfo().noquote() << QStringLiteral( "add-zone: track %1 slot %2: zone %3 with %4 layer(s)" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( id ).arg( layers_.size() );
    return { true, new SRemoveZoneAction( trackPath_, slotIndex_, id ) };
}

void SAddZoneAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    writeAttributes( elem, zone_ );
    for( const SSamplerAttrList &l : layers_ ) {
        QDomElement le = elem.ownerDocument().createElement( QStringLiteral( "layer" ) );
        writeAttributes( le, l );
        elem.appendChild( le );
    }
}

bool SAddZoneAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    zone_ = valueAttributes( elem, {} );
    layers_.clear();
    for( QDomElement c = elem.firstChildElement( QStringLiteral( "layer" ) ); !c.isNull();
         c = c.nextSiblingElement( QStringLiteral( "layer" ) ) )
        layers_.append( valueAttributes( c, {} ) );
    return true;
}

QStringList SAddZoneAction::knownAttributes() const
{
    // `id`: an explicit zone id (QBX-265), validated by the core's wrapper.
    return addressAttributes() + names( twSamplerScope::Zone ) + QStringList{ QStringLiteral( "id" ) };
}

// =========================================================== remove-zone ====

SApplyResult SRemoveZoneAction::apply( SProject *project )
{
    QString why;
    if( zone_ == 0 ) return refuse( QStringLiteral( "no zone id" ) );
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    std::string xml, err;
    std::size_t at = 0;
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            return m.removeZone( zone_, &xml, &at, &err );
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
    qInfo().noquote() << QStringLiteral( "remove-zone: track %1 slot %2: zone %3" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( zone_ );
    return { true, new SReinsertZoneAction( trackPath_, slotIndex_, zone_, at, xml ) };
}

void SRemoveZoneAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "zone", (qulonglong)zone_ );
}

bool SRemoveZoneAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    zone_ = (std::uint32_t)elem.attribute( "zone", "0" ).toULongLong();
    return true;
}

QStringList SRemoveZoneAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "zone" );
}

// ========================================================= reinsert-zone ====

SApplyResult SReinsertZoneAction::apply( SProject *project )
{
    QString why;
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    std::string err;
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            if( !m.reinsertZone( xml_, at_, &err ) ) return false;
            if( !m.hasZone( zone_ ) ) {
                err = "the reinserted zone does not carry id " + std::to_string( zone_ );
                return false;
            }
            return true;
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
    return { true, new SRemoveZoneAction( trackPath_, slotIndex_, zone_ ) };
}

void SReinsertZoneAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "zone", (qulonglong)zone_ );
    elem.setAttribute( "at", (qulonglong)at_ );
    writeText( elem, xml_ );
}

bool SReinsertZoneAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    zone_ = (std::uint32_t)elem.attribute( "zone", "0" ).toULongLong();
    at_   = (std::size_t)elem.attribute( "at", "0" ).toULongLong();
    xml_  = elem.text().toStdString();
    return true;
}

QStringList SReinsertZoneAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "zone" ) << QStringLiteral( "at" );
}

// ============================================================= add-layer ====

SApplyResult SAddLayerAction::apply( SProject *project )
{
    QString why;
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );

    if( createdId_ != 0 ) {   // the redo, as add-zone's
        std::string err;
        if( !prog->edit( [&]( twSamplerProgramModel &m ) {
                return m.reinsertLayer( createdXml_, createdZone_, createdIndex_, &err );
            }, &why ) )
            return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
        return { true, new SRemoveLayerAction( trackPath_, slotIndex_, createdId_ ) };
    }

    std::vector<std::uint32_t> zone;
    if( !parseIdList( zone_, zone ) || zone.size() != 1 )
        return refuse( QStringLiteral( "zone=\"%1\" is not one zone id" ).arg( zone_ ) );

    std::uint32_t id = 0;
    std::string   err;
    const twSamplerAttrs attrs = toCore( layer_ );
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            id = m.addLayer( zone.front(), attrs, &err );
            return id != 0 && m.layerXml( id, &createdXml_, &createdZone_, &createdIndex_ );
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
    createdId_ = id;
    qInfo().noquote() << QStringLiteral( "add-layer: track %1 slot %2: layer %3 in zone %4" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( id ).arg( zone.front() );
    return { true, new SRemoveLayerAction( trackPath_, slotIndex_, id ) };
}

void SAddLayerAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "zone", zone_ );
    writeAttributes( elem, layer_ );
}

bool SAddLayerAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    zone_  = elem.attribute( "zone" );
    layer_ = valueAttributes( elem, { QStringLiteral( "zone" ) } );
    return true;
}

QStringList SAddLayerAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "zone" ) << names( twSamplerScope::Layer );
}

// ========================================================== remove-layer ====

SApplyResult SRemoveLayerAction::apply( SProject *project )
{
    QString why;
    if( layer_ == 0 ) return refuse( QStringLiteral( "no layer id" ) );
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    std::string   xml, err;
    std::uint32_t zone = 0;
    std::size_t   at   = 0;
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            return m.removeLayer( layer_, &xml, &zone, &at, &err );
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
    return { true, new SReinsertLayerAction( trackPath_, slotIndex_, layer_, zone, at, xml ) };
}

void SRemoveLayerAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "layer", (qulonglong)layer_ );
}

bool SRemoveLayerAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    layer_ = (std::uint32_t)elem.attribute( "layer", "0" ).toULongLong();
    return true;
}

QStringList SRemoveLayerAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "layer" );
}

// ======================================================== reinsert-layer ====

SApplyResult SReinsertLayerAction::apply( SProject *project )
{
    QString why;
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    std::string err;
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            if( !m.reinsertLayer( xml_, zone_, at_, &err ) ) return false;
            if( !m.hasLayer( layer_ ) ) {
                err = "the reinserted layer does not carry id " + std::to_string( layer_ );
                return false;
            }
            return true;
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
    return { true, new SRemoveLayerAction( trackPath_, slotIndex_, layer_ ) };
}

void SReinsertLayerAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "layer", (qulonglong)layer_ );
    elem.setAttribute( "zone", (qulonglong)zone_ );
    elem.setAttribute( "at", (qulonglong)at_ );
    writeText( elem, xml_ );
}

bool SReinsertLayerAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    layer_ = (std::uint32_t)elem.attribute( "layer", "0" ).toULongLong();
    zone_  = (std::uint32_t)elem.attribute( "zone", "0" ).toULongLong();
    at_    = (std::size_t)elem.attribute( "at", "0" ).toULongLong();
    xml_   = elem.text().toStdString();
    return true;
}

QStringList SReinsertLayerAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "layer" ) << QStringLiteral( "zone" )
                               << QStringLiteral( "at" );
}

// ===================================== set-zone / set-layer / set-sampler-program ===

QString SSetSamplerValuesAction::name() const
{
    switch( scope_ ) {
    case Scope::Program: return QStringLiteral( "set-sampler-program" );
    case Scope::Zone:    return QStringLiteral( "set-zone" );
    case Scope::Layer:   return QStringLiteral( "set-layer" );
    }
    return QStringLiteral( "set-zone" );
}

SApplyResult SSetSamplerValuesAction::apply( SProject *project )
{
    QString why;
    const twSamplerScope scope = scopeOf( scope_ );

    // The targets.
    bool all = false;
    std::vector<std::uint32_t> ids;
    if( scope_ != Scope::Program ) {
        if( selectorAttr_.isEmpty() )
            return refuse( scope_ == Scope::Zone
                               ? QStringLiteral( "no zone= or zones= (an id, a list or \"all\")" )
                               : QStringLiteral( "no layer= or layers=" ) );
        if( selector_ == QLatin1String( "all" ) ) {
            if( scope_ != Scope::Zone )
                return refuse( QStringLiteral( "the ALL scope is a zone scope (DESIGN §2.1)" ) );
            all = true;
        } else if( !parseIdList( selector_, ids ) ||
                   ( ( selectorAttr_ == QLatin1String( "zone" ) ||
                       selectorAttr_ == QLatin1String( "layer" ) ) && ids.size() != 1 ) ) {
            return refuse( QStringLiteral( "%1=\"%2\" is not %3" )
                               .arg( selectorAttr_, selector_,
                                     selectorAttr_.endsWith( QLatin1Char( 's' ) )
                                         ? QStringLiteral( "a list of ids or \"all\"" )
                                         : QStringLiteral( "one id or \"all\"" ) ) );
        }
    }

    // The edits.
    std::vector<twSamplerEdit> edits;
    for( const auto &a : values_ ) {
        twSamplerEdit e;
        if( !toEdit( scope, a.first, a.second, e, &why ) ) return refuse( why );
        edits.push_back( std::move( e ) );
    }
    if( edits.empty() ) return refuse( QStringLiteral( "nothing to set" ) );

    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );

    std::vector<twSamplerPrior> prior;
    std::string err;
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            return m.set( scope, all, ids, edits, &prior, &err );
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );

    return { true, new SRestoreSamplerValuesAction( trackPath_, slotIndex_, scopeText( scope ),
                                                    fromCore( prior ) ) };
}

void SSetSamplerValuesAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    if( !selectorAttr_.isEmpty() ) elem.setAttribute( selectorAttr_, selector_ );
    writeAttributes( elem, values_ );
}

bool SSetSamplerValuesAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    QStringList selectors;
    if( scope_ == Scope::Zone )  selectors = { QStringLiteral( "zone" ), QStringLiteral( "zones" ) };
    if( scope_ == Scope::Layer ) selectors = { QStringLiteral( "layer" ), QStringLiteral( "layers" ) };
    selectorAttr_.clear();
    selector_.clear();
    for( const QString &s : selectors ) {
        if( !elem.hasAttribute( s ) ) continue;
        if( !selectorAttr_.isEmpty() ) return false;   // zone= AND zones= is ambiguous
        selectorAttr_ = s;
        selector_     = elem.attribute( s );
    }
    values_ = valueAttributes( elem, selectors );
    return true;
}

QStringList SSetSamplerValuesAction::knownAttributes() const
{
    QStringList out = addressAttributes();
    if( scope_ == Scope::Zone )  out << QStringLiteral( "zone" ) << QStringLiteral( "zones" );
    if( scope_ == Scope::Layer ) out << QStringLiteral( "layer" ) << QStringLiteral( "layers" );
    return out + names( scopeOf( scope_ ) );
}

// =================================================== restore-sampler-values ===

SApplyResult SRestoreSamplerValuesAction::apply( SProject *project )
{
    QString why;
    twSamplerScope scope;
    if( !scopeFromText( scope_, scope ) )
        return refuse( QStringLiteral( "scope=\"%1\" is not program, zone or layer" ).arg( scope_ ) );
    std::vector<twSamplerPrior> prior;
    for( const Prior &p : prior_ ) prior.push_back( { p.id, p.attr.toStdString(), p.value.toStdString() } );

    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    std::vector<twSamplerPrior> replaced;
    std::string err;
    if( !prog->edit( [&]( twSamplerProgramModel &m ) {
            return m.restore( scope, prior, &replaced, &err );
        }, &why ) )
        return refuse( why.isEmpty() ? QString::fromStdString( err ) : why );
    return { true, new SRestoreSamplerValuesAction( trackPath_, slotIndex_, scope_,
                                                    fromCore( replaced ) ) };
}

void SRestoreSamplerValuesAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "scope", scope_ );
    for( const Prior &p : prior_ ) {
        QDomElement pe = elem.ownerDocument().createElement( QStringLiteral( "prior" ) );
        pe.setAttribute( "id", (qulonglong)p.id );
        pe.setAttribute( "attr", p.attr );
        pe.setAttribute( "value", p.value );
        elem.appendChild( pe );
    }
}

bool SRestoreSamplerValuesAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    scope_ = elem.attribute( "scope", "zone" );
    prior_.clear();
    for( QDomElement c = elem.firstChildElement( QStringLiteral( "prior" ) ); !c.isNull();
         c = c.nextSiblingElement( QStringLiteral( "prior" ) ) )
        prior_.append( { (std::uint32_t)c.attribute( "id", "0" ).toULongLong(), c.attribute( "attr" ),
                         c.attribute( "value" ) } );
    return true;
}

QStringList SRestoreSamplerValuesAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "scope" );
}

// ========================================================== select-zones ====

SApplyResult SSelectZonesAction::apply( SProject *project )
{
    QString why;
    const bool replace = mode_ == QLatin1String( "replace" );
    const bool add     = mode_ == QLatin1String( "add" );
    const bool toggle  = mode_ == QLatin1String( "toggle" );
    if( !replace && !add && !toggle )
        return refuse( QStringLiteral( "mode=\"%1\" is not replace, add or toggle" ).arg( mode_ ) );

    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );

    QVector<std::uint32_t> named;
    if( zones_ == QLatin1String( "all" ) ) {
        for( std::uint32_t id : prog->model().zoneIds() ) named.append( id );
    } else if( !zones_.trimmed().isEmpty() ) {
        std::vector<std::uint32_t> ids;
        if( !parseIdList( zones_, ids ) )
            return refuse( QStringLiteral( "zones=\"%1\" is not a list of zone ids, \"all\" or "
                                           "empty" ).arg( zones_ ) );
        for( std::uint32_t id : ids ) {
            if( !prog->model().hasZone( id ) )
                return refuse( QStringLiteral( "no zone with id %1" ).arg( id ) );
            if( !named.contains( id ) ) named.append( id );
        }
    } else if( !replace ) {
        return refuse( QStringLiteral( "an empty zones= only makes sense with mode=\"replace\"" ) );
    }

    const QVector<std::uint32_t> before = prog->rawSelection();
    QVector<std::uint32_t> after;
    if( replace ) {
        after = named;
    } else {
        after = prog->selectedZones();
        for( std::uint32_t id : named ) {
            const int at = after.indexOf( id );
            if( at < 0 ) after.append( id );
            else if( toggle ) after.remove( at );
        }
    }
    prog->setSelection( after );
    qInfo().noquote() << QStringLiteral( "select-zones: track %1 slot %2: selected [%3]" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( idListText( after ) );
    return { true, new SSelectZonesAction( trackPath_, slotIndex_, idListText( before ),
                                           QStringLiteral( "replace" ) ) };
}

void SSelectZonesAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "zones", zones_ );
    elem.setAttribute( "mode", mode_ );
}

bool SSelectZonesAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    zones_ = elem.attribute( "zones" );
    mode_  = elem.attribute( "mode", "replace" );
    return true;
}

QStringList SSelectZonesAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "zones" ) << QStringLiteral( "mode" );
}

// ========================================================= registration =====

static const bool s_reg_sampler_verbs = [] {
    SActionRegistry &r = SActionRegistry::instance();
    r.registerType( QStringLiteral( "add-zone" ), [] { return new SAddZoneAction; } );
    r.registerType( QStringLiteral( "remove-zone" ), [] { return new SRemoveZoneAction; } );
    r.registerType( QStringLiteral( "reinsert-zone" ), [] { return new SReinsertZoneAction; } );
    r.registerType( QStringLiteral( "add-layer" ), [] { return new SAddLayerAction; } );
    r.registerType( QStringLiteral( "remove-layer" ), [] { return new SRemoveLayerAction; } );
    r.registerType( QStringLiteral( "reinsert-layer" ), [] { return new SReinsertLayerAction; } );
    r.registerType( QStringLiteral( "set-zone" ), [] {
        return new SSetSamplerValuesAction( SSetSamplerValuesAction::Scope::Zone );
    } );
    r.registerType( QStringLiteral( "set-layer" ), [] {
        return new SSetSamplerValuesAction( SSetSamplerValuesAction::Scope::Layer );
    } );
    r.registerType( QStringLiteral( "set-sampler-program" ), [] {
        return new SSetSamplerValuesAction( SSetSamplerValuesAction::Scope::Program );
    } );
    r.registerType( QStringLiteral( "restore-sampler-values" ),
                    [] { return new SRestoreSamplerValuesAction; } );
    r.registerType( QStringLiteral( "select-zones" ), [] { return new SSelectZonesAction; } );
    return true;
}();
