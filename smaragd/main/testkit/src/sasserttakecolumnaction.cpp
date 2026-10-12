#include "app/testkit/sasserttakecolumnaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/stakestack.h"
#include "tw/events/twcompmap.h"

using namespace strackpath;

SApplyResult SAssertTakeColumnAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    // PARSE FIRST: parseInto() is what sets pathRoot_ (assert-clip-window's
    // own trap, same reasoning).
    const QList<int> idx = parseInto( pathRoot_, clip_ );
    SObject *root = splacements::rootNamed( project, pathRoot_ );
    SLink *link = root ? splacements::placementAt( root, idx ) : nullptr;
    if( !link ) {
        qWarning() << "assert-take-column: no clip at" << clip_;
        return { false, nullptr };
    }

    STakeStack *column = stakes::columnOfLink( link );
    // The SHAPE is read from the placement itself, never from the resolver:
    // that is the whole point of this verb. stakes::isDirectColumn is that
    // question, and asking it there rather than repeating the dynamic_cast here
    // keeps ONE spelling of it -- which matters now that remove-sample no
    // longer uses it to decide what it may pin (QBX-155).
    const bool isDirect = stakes::isDirectColumn( link );
    const QString gotShape = !column ? QStringLiteral( "none" )
                           : isDirect ? QStringLiteral( "direct" )
                                      : QStringLiteral( "wrapped" );
    const int gotTakes  = column ? column->nTakes() : 0;
    const int gotActive = column ? column->activeTakeIndex() : -1;
    const int gotPlace  = column ? column->refCount() : 0;
    // Deliberately the LINK'S object, not the column: see the header.
    const int gotLinkRefs = link->getSObject().refCount();

    // The map as text, in the same spelling the attribute takes.
    QString gotComp;
    if( column ) {
        for( const twCompSegment &sg : column->compMap().segments() ) {
            if( !gotComp.isEmpty() ) gotComp += ";";
            gotComp += QString::number( sg.at ) + ":" + QString::number( sg.take );
            if( sg.xfade ) gotComp += ":" + QString::number( sg.xfade );
        }
    }

    // EVERY TAKE'S EXTENT, in the same spelling the attribute takes
    // (plan 53 §4.1).
    QString gotExtents;
    if( column ) {
        for( int i = 0; i < column->nTakes(); ++i ) {
            if( !gotExtents.isEmpty() ) gotExtents += ";";
            gotExtents += QString::number( column->takeExtentStart( i ) ) + "-"
                        + QString::number( column->takeExtentEnd( i ) );
        }
    }
    const qint64 gotDuration =
        column ? (qint64) column->getDurationBlocking() : -1;

    const QString detail =
        QString( "clip %1: shape=%2 takes=%3 activeTake=%4 placements=%5" )
            .arg( clip_ ).arg( gotShape ).arg( gotTakes ).arg( gotActive )
            .arg( gotPlace )
        + QString( " comp=[%1] extents=[%2] duration=%3" )
              .arg( gotComp ).arg( gotExtents ).arg( gotDuration );

    if( !shape_.isEmpty()
        && shape_.compare( gotShape, Qt::CaseInsensitive ) != 0 ) {
        qWarning() << "assert-take-column FAILED: shape expected" << shape_
                   << "-" << detail;
        return { false, nullptr };
    }
    if( hasComp_ && comp_ != gotComp ) {
        qWarning() << "assert-take-column FAILED: comp expected" << comp_
                   << "-" << detail;
        return { false, nullptr };
    }
    if( hasExtents_ && extents_ != gotExtents ) {
        qWarning() << "assert-take-column FAILED: extents expected" << extents_
                   << "-" << detail;
        return { false, nullptr };
    }
    if( duration_ >= 0 && duration_ != gotDuration ) {
        qWarning() << "assert-take-column FAILED: duration expected"
                   << duration_ << "-" << detail;
        return { false, nullptr };
    }
    if( takeAtPos_ >= 0 && column ) {
        const int gotAt = column->takeIndexAt( takeAtPos_ );
        if( gotAt != expectTake_ ) {
            qWarning() << "assert-take-column FAILED: takeAt" << takeAtPos_
                       << "expected" << expectTake_ << "got" << gotAt
                       << "-" << detail;
            return { false, nullptr };
        }
    }
    struct Check { const char *what; int want, got; };
    const Check checks[] = {
        { "takes",      takes_,      gotTakes },
        { "activeTake", activeTake_, gotActive },
        { "placements", placements_, gotPlace },
        { "linkObjectRefs", linkObjectRefs_, gotLinkRefs },
    };
    for( const Check &c : checks ) {
        // CAUTION, and the reason this is a comment rather than a refactor:
        // the "is it asserted" test dispatches on the attribute name's FIRST
        // LETTER, because activeTake is the one field whose -1 is a legal
        // value and so needs -2 as its sentinel. A future field whose name
        // starts with 'a' would silently inherit that sentinel and stop being
        // checked at -1. linkObjectRefs starts with 'l', so it is fine.
        if( c.want == -1 && c.what[0] != 'a' ) continue;   // -1 = not asserted
        if( c.what[0] == 'a' && c.want == -2 ) continue;   // activeTake sentinel
        if( c.want != c.got ) {
            qWarning() << "assert-take-column FAILED:" << c.what << "expected"
                       << c.want << "got" << c.got << "-" << detail;
            return { false, nullptr };
        }
    }
    qDebug() << "assert-take-column: OK -" << detail;
    return { true, nullptr };
}

void SAssertTakeColumnAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clip_ );
    if( !shape_.isEmpty() ) elem.setAttribute( "shape", shape_ );
    elem.setAttribute( "takes", takes_ );
    elem.setAttribute( "activeTake", activeTake_ );
    elem.setAttribute( "placements", placements_ );
    if( linkObjectRefs_ >= 0 ) elem.setAttribute( "linkObjectRefs", linkObjectRefs_ );
    if( hasComp_ ) elem.setAttribute( "comp", comp_ );
    if( hasExtents_ ) elem.setAttribute( "extents", extents_ );
    if( duration_ >= 0 ) elem.setAttribute( "duration",
                                            QString::number( duration_ ) );
    if( takeAtPos_ >= 0 ) {
        elem.setAttribute( "takeAtPos", QString::number( takeAtPos_ ) );
        elem.setAttribute( "expectTake", expectTake_ );
    }
}

bool SAssertTakeColumnAction::readXml( const QDomElement &elem, int )
{
    clip_       = elem.attribute( "clip", "" );
    shape_      = elem.attribute( "shape", "" );
    takes_      = elem.attribute( "takes", "-1" ).toInt();
    activeTake_ = elem.attribute( "activeTake", "-2" ).toInt();
    placements_ = elem.attribute( "placements", "-1" ).toInt();
    linkObjectRefs_ = elem.attribute( "linkObjectRefs", "-1" ).toInt();
    hasComp_    = elem.hasAttribute( "comp" );
    comp_       = elem.attribute( "comp", "" );
    hasExtents_ = elem.hasAttribute( "extents" );
    extents_    = elem.attribute( "extents", "" );
    duration_   = elem.attribute( "duration", "-1" ).toLongLong();
    takeAtPos_  = elem.attribute( "takeAtPos", "-1" ).toLongLong();
    expectTake_ = elem.attribute( "expectTake", "-2" ).toInt();
    return true;
}

static const bool s_reg_assert_take_column = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-take-column" ),
        []{ return new SAssertTakeColumnAction; } ), true );
