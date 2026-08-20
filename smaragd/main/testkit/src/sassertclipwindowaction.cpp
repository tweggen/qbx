#include "app/testkit/sassertclipwindowaction.h"

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"

using namespace strackpath;

SApplyResult SAssertClipWindowAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    // PARSE FIRST: parseInto() is what SETS pathRoot_ from a qualified
    // text path, so resolving the root before it runs reads an empty
    // root and silently addresses the MASTER.
    const QList<int> idx_ = parseInto( pathRoot_, clip_ );
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = mixer ? splacements::placementAt( mixer, idx_ )
    : nullptr;
    if( !link ) {
        qWarning() << "assert-clip-window: no clip at" << clip_;
        return { false, nullptr };
    }
    SObject *obj = &link->getSObject();
    // A stack presents the addressed take through the generic take seam.
    if( SClipWindow *w = obj->windowTakeAt( take_ ) ) obj = &w->asObject();
    SClipWindow *win = SClipWindow::of( obj );

    const qint64 gotStart = (qint64) link->getStartTime();
    const qint64 gotDur   = (qint64) ( win ? win->duration()
                                           : obj->getDuration() );
    const qint64 gotLoop  = win ? (qint64) win->loopLength() : -1;
    const qint64 gotSlip  = win ? (qint64) win->startOffset() : -1;
    const QString gotTb =
        ( link->getTimebase() == SLink::Timebase::Beats ) ? "beats" : "time";

    const QString detail =
        QString( "clip %1: startTime=%2 duration=%3 loopLength=%4 "
                 "startOffset=%5 timebase=%6" )
            .arg( clip_ ).arg( gotStart ).arg( gotDur ).arg( gotLoop )
            .arg( gotSlip ).arg( gotTb );

    struct Check { const char *what; qint64 want, got; };
    const Check checks[] = {
        { "startTime",   startTime_,   gotStart },
        { "duration",    duration_,    gotDur },
        { "loopLength",  loopLength_,  gotLoop },
        { "startOffset", startOffset_, gotSlip },
    };
    for( const Check &c : checks ) {
        if( c.want < 0 ) continue;
        if( c.want != c.got ) {
            qWarning() << "assert-clip-window FAILED:" << c.what << "expected"
                       << c.want << "got" << c.got << "-" << detail;
            return { false, nullptr };
        }
    }
    if( !timebase_.isEmpty()
        && timebase_.compare( gotTb, Qt::CaseInsensitive ) != 0 ) {
        qWarning() << "assert-clip-window FAILED: timebase expected"
                   << timebase_ << "-" << detail;
        return { false, nullptr };
    }
    qDebug() << "assert-clip-window: OK -" << detail;
    return { true, nullptr };   // assertions are not undoable
}

void SAssertClipWindowAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "clip", clip_ );
    elem.setAttribute( "startTime", QString::number( startTime_ ) );
    elem.setAttribute( "duration", QString::number( duration_ ) );
    elem.setAttribute( "loopLength", QString::number( loopLength_ ) );
    elem.setAttribute( "startOffset", QString::number( startOffset_ ) );
    if( !timebase_.isEmpty() ) elem.setAttribute( "timebase", timebase_ );
    elem.setAttribute( "take", take_ );
}

bool SAssertClipWindowAction::readXml( const QDomElement &elem, int )
{
    clip_ = elem.attribute( "clip", "" );
    startTime_ = elem.attribute( "startTime", "-1" ).toLongLong();
    duration_ = elem.attribute( "duration", "-1" ).toLongLong();
    loopLength_ = elem.attribute( "loopLength", "-1" ).toLongLong();
    startOffset_ = elem.attribute( "startOffset", "-1" ).toLongLong();
    timebase_ = elem.attribute( "timebase", "" );
    take_ = elem.attribute( "take", "-1" ).toInt();
    return true;
}

static const bool s_reg_assert_clip_window = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-clip-window" ),
        []{ return new SAssertClipWindowAction; } ), true );
