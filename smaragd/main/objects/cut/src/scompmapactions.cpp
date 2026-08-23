#include "app/objects/cut/scompmapactions.h"

#include <QDomElement>
#include <QDebug>

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"

using namespace strackpath;

SCompMapAction::SCompMapAction( Op op, const QString &clip, qint64 at, int take,
                                qint64 xfade, qint64 to )
    : op_( op ), clip_( clip ), at_( at ), take_( take ), xfade_( xfade ),
      to_( to )
{
}

SCompMapAction *SCompMapAction::wholeMap( const QString &clip,
                                          const twCompMap &map )
{
    SCompMapAction *a = new SCompMapAction();
    a->op_ = Op::SetWhole;
    a->clip_ = clip;
    a->whole_ = map;
    return a;
}

QString SCompMapAction::name() const
{
    switch( op_ ) {
    case Op::SetSegment:    return QStringLiteral( "set-comp-segment" );
    case Op::RemoveSegment: return QStringLiteral( "remove-comp-segment" );
    case Op::MoveBoundary:  return QStringLiteral( "move-comp-boundary" );
    case Op::SetXfade:      return QStringLiteral( "set-comp-xfade" );
    case Op::SetWhole:      break;
    }
    return QStringLiteral( "set-comp-map" );
}

SApplyResult SCompMapAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    // PARSE FIRST: parseInto() is what sets pathRoot_ (assert-clip-window's
    // trap, and every take verb's since).
    const QList<int> idx = parseInto( pathRoot_, clip_ );
    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SLink *link = root ? splacements::placementAt( root, idx ) : nullptr;
    // BOTH SHAPES, through the one resolver (proposal 42).
    STakeStack *column = stakes::columnOfLink( link );
    if( !column ) {
        qWarning() << name() << ": no take column at" << clip_;
        return { false, nullptr };
    }

    const twCompMap before = column->compMap();
    std::vector<twCompSegment> segs = before.segments();

    switch( op_ ) {
    case Op::SetWhole:
        segs = whole_.segments();
        break;
    case Op::SetSegment: {
        if( take_ < 0 || take_ >= column->nTakes() ) {
            qWarning() << name() << ": take" << take_ << "out of range";
            return { false, nullptr };
        }
        // normalized() REPLACES an existing segment at the same position, so
        // appending is the whole edit — see twCompMap for why that is a fold
        // and not std::unique.
        twCompSegment s; s.at = at_; s.take = take_; s.xfade = xfade_;
        segs.push_back( s );
        break;
    }
    case Op::RemoveSegment: {
        std::vector<twCompSegment> keep;
        for( const twCompSegment &s : segs )
            if( s.at != at_ ) keep.push_back( s );
        if( keep.size() == segs.size() ) {
            qWarning() << name() << ": no segment at" << at_;
            return { false, nullptr };
        }
        segs = keep;
        break;
    }
    case Op::MoveBoundary: {
        bool found = false;
        for( twCompSegment &s : segs )
            if( s.at == at_ ) { s.at = to_; found = true; break; }
        if( !found ) {
            qWarning() << name() << ": no boundary at" << at_;
            return { false, nullptr };
        }
        break;
    }
    case Op::SetXfade: {
        bool found = false;
        for( twCompSegment &s : segs )
            if( s.at == at_ ) { s.xfade = xfade_; found = true; break; }
        if( !found ) {
            qWarning() << name() << ": no boundary at" << at_;
            return { false, nullptr };
        }
        break;
    }
    }

    column->setCompMap( twCompMap( std::move( segs ) ) );
    // THE INVERSE IS THE WHOLE PREVIOUS MAP. A per-segment inverse would have
    // to name a segment, and a segment's only identity is its position -- the
    // very thing `move-comp-boundary` changes.
    return { true, wholeMap( clip_, before ) };
}

void SCompMapAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clip_ );
    switch( op_ ) {
    case Op::SetSegment:
        elem.setAttribute( "at", QString::number( at_ ) );
        elem.setAttribute( "take", take_ );
        if( xfade_ ) elem.setAttribute( "xfade", QString::number( xfade_ ) );
        break;
    case Op::RemoveSegment:
        elem.setAttribute( "at", QString::number( at_ ) );
        break;
    case Op::MoveBoundary:
        elem.setAttribute( "at", QString::number( at_ ) );
        elem.setAttribute( "to", QString::number( to_ ) );
        break;
    case Op::SetXfade:
        elem.setAttribute( "at", QString::number( at_ ) );
        elem.setAttribute( "xfade", QString::number( xfade_ ) );
        break;
    case Op::SetWhole:
        // The whole-map form is an INVERSE, never scripted: it is serialized
        // as its segments so an action log stays readable.
        for( const twCompSegment &s : whole_.segments() ) {
            QDomElement e = elem.ownerDocument().createElement( "seg" );
            e.setAttribute( "at", QString::number( s.at ) );
            e.setAttribute( "take", s.take );
            if( s.xfade ) e.setAttribute( "xfade", QString::number( s.xfade ) );
            elem.appendChild( e );
        }
        break;
    }
}

bool SCompMapAction::readXml( const QDomElement &elem, int )
{
    clip_  = elem.attribute( "clip", "" );
    at_    = elem.attribute( "at", "0" ).toLongLong();
    take_  = elem.attribute( "take", "0" ).toInt();
    xfade_ = elem.attribute( "xfade", "0" ).toLongLong();
    to_    = elem.attribute( "to", "0" ).toLongLong();
    const QString tag = elem.tagName();
    if( tag == QStringLiteral( "remove-comp-segment" ) ) op_ = Op::RemoveSegment;
    else if( tag == QStringLiteral( "move-comp-boundary" ) ) op_ = Op::MoveBoundary;
    else if( tag == QStringLiteral( "set-comp-xfade" ) ) op_ = Op::SetXfade;
    else if( tag == QStringLiteral( "set-comp-map" ) ) {
        op_ = Op::SetWhole;
        std::vector<twCompSegment> segs;
        for( QDomElement e = elem.firstChildElement( "seg" ); !e.isNull();
             e = e.nextSiblingElement( "seg" ) ) {
            twCompSegment s;
            s.at    = e.attribute( "at", "0" ).toLongLong();
            s.take  = e.attribute( "take", "0" ).toInt();
            s.xfade = e.attribute( "xfade", "0" ).toLongLong();
            segs.push_back( s );
        }
        whole_ = twCompMap( std::move( segs ) );
    }
    else op_ = Op::SetSegment;
    return true;
}

static SAction *makeOp( SCompMapAction::Op op )
{
    SCompMapAction *a = new SCompMapAction();
    // readXml re-derives the op from the tag, so this only has to produce an
    // object of the right type; the op it starts with is immaterial.
    (void) op;
    return a;
}

static const bool s_reg_comp_map = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-comp-segment" ),
        []{ return makeOp( SCompMapAction::Op::SetSegment ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "remove-comp-segment" ),
        []{ return makeOp( SCompMapAction::Op::RemoveSegment ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "move-comp-boundary" ),
        []{ return makeOp( SCompMapAction::Op::MoveBoundary ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-comp-xfade" ),
        []{ return makeOp( SCompMapAction::Op::SetXfade ); } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "set-comp-map" ),
        []{ return makeOp( SCompMapAction::Op::SetWhole ); } ),
    true );
