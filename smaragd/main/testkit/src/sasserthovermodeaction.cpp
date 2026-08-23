#include "app/testkit/sasserthovermodeaction.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"

SApplyResult SAssertHoverModeAction::apply( SProject * )
{
    SMainWindow *win = NULL;
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( ( win = qobject_cast<SMainWindow*>( w ) ) ) break;
    if( !win ) {
        qWarning() << "assert-hover-mode: no main window";
        return { false, nullptr };
    }
    if( !win->hoverClipEdge( track_, clip_, grabWhere_ ) ) {
        qWarning() << "assert-hover-mode: no clip at track" << track_
                   << "index" << clip_;
        return { false, nullptr };
    }
    const QString got = SApplication::app().getStatusMode();
    if( got != mode_ ) {
        qWarning() << "assert-hover-mode FAILED: expected" << mode_
                   << "got" << got;
        return { false, nullptr };
    }
    qDebug() << "assert-hover-mode: OK -" << got;
    return { true, nullptr };
}

void SAssertHoverModeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "clip", clip_ );
    elem.setAttribute( "edge", grabWhere_ == 1 ? "end"
                             : grabWhere_ == 2 ? "body"
                             : grabWhere_ == 3 ? "tag"
                             : grabWhere_ == 4 ? "fadein"
                             : grabWhere_ == 5 ? "fadeout" : "start" );
    elem.setAttribute( "mode", mode_ );
}

bool SAssertHoverModeAction::readXml( const QDomElement &elem, int )
{
    track_ = elem.attribute( "track", "0" ).toInt();
    clip_  = elem.attribute( "clip", "0" ).toInt();
    const QString edge = elem.attribute( "edge", "body" );
    if( edge == "end" )           grabWhere_ = 1;
    else if( edge == "body" )     grabWhere_ = 2;
    else if( edge == "tag" )      grabWhere_ = 3;
    else if( edge == "fadein" )   grabWhere_ = 4;
    else if( edge == "fadeout" )  grabWhere_ = 5;
    else                          grabWhere_ = 0;
    mode_ = elem.attribute( "mode", "" );
    return true;
}

static const bool s_reg_assert_hover_mode = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-hover-mode" ),
        []{ return new SAssertHoverModeAction; } ), true );
