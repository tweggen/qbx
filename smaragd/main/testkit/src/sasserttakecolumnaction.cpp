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
    // that is the whole point of this verb.
    const bool isDirect =
        ( dynamic_cast<STakeStack *>( &link->getSObject() ) != nullptr );
    const QString gotShape = !column ? QStringLiteral( "none" )
                           : isDirect ? QStringLiteral( "direct" )
                                      : QStringLiteral( "wrapped" );
    const int gotTakes  = column ? column->nTakes() : 0;
    const int gotActive = column ? column->activeTakeIndex() : -1;
    const int gotPlace  = column ? column->refCount() : 0;

    const QString detail =
        QString( "clip %1: shape=%2 takes=%3 activeTake=%4 placements=%5" )
            .arg( clip_ ).arg( gotShape ).arg( gotTakes ).arg( gotActive )
            .arg( gotPlace );

    if( !shape_.isEmpty()
        && shape_.compare( gotShape, Qt::CaseInsensitive ) != 0 ) {
        qWarning() << "assert-take-column FAILED: shape expected" << shape_
                   << "-" << detail;
        return { false, nullptr };
    }
    struct Check { const char *what; int want, got; };
    const Check checks[] = {
        { "takes",      takes_,      gotTakes },
        { "activeTake", activeTake_, gotActive },
        { "placements", placements_, gotPlace },
    };
    for( const Check &c : checks ) {
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
}

bool SAssertTakeColumnAction::readXml( const QDomElement &elem, int )
{
    clip_       = elem.attribute( "clip", "" );
    shape_      = elem.attribute( "shape", "" );
    takes_      = elem.attribute( "takes", "-1" ).toInt();
    activeTake_ = elem.attribute( "activeTake", "-2" ).toInt();
    placements_ = elem.attribute( "placements", "-1" ).toInt();
    return true;
}

static const bool s_reg_assert_take_column = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-take-column" ),
        []{ return new SAssertTakeColumnAction; } ), true );
