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
    // WHICH question this verb is asking depends on whether a take was NAMED.
    // With `take=`, the addressed take (which since proposal 42 M2 the seam
    // reaches through a WINDOW over a column too — it used to be silently
    // ignored there). Without it, the window whose PARAMETERS the placement
    // carries: the clip itself, or a direct column's active take.
    if( SClipWindow *w = ( take_ >= 0 ? obj->windowTakeAt( take_ )
                                      : SClipWindow::parametersOf( *obj ) ) )
        obj = &w->asObject();
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

    // A NEGATIVE startOffset IS A REAL VALUE, not the sentinel. A clip may
    // legitimately open with LEADING SILENCE and have its data start later
    // (proposal 23), which is a negative slip -- and it is what a pass snapped
    // EARLIER than its capture's pre-roll handle produces (plan 53 §3.5). So
    // this verb, the only one that reads the slip at all, could not assert the
    // one value that most needs asserting: every negative expectation was
    // silently dropped by the `want < 0` sentinel below.
    //
    // `-1` STAYS unset, so every existing spelling and the round-trip fixture
    // are unchanged; any OTHER negative value is an expectation. -1 is a
    // meaningless slip, and keeping it as the sentinel is what makes this
    // widening cost nothing.
    const bool checkSlip = startOffset_ != -1;

    struct Check { const char *what; qint64 want, got; bool force; };
    const Check checks[] = {
        { "startTime",   startTime_,   gotStart, false },
        { "duration",    duration_,    gotDur,   false },
        { "loopLength",  loopLength_,  gotLoop,  false },
        { "startOffset", startOffset_, gotSlip,  checkSlip },
    };
    for( const Check &c : checks ) {
        if( c.want < 0 && !c.force ) continue;
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
