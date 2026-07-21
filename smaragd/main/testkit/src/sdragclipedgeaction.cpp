#include "app/testkit/sdragclipedgeaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include <QApplication>
#include <QDomElement>
#include <QDebug>

SDragClipEdgeAction::SDragClipEdgeAction( int track, int clip, bool grabEnd,
                                          offset_t toTime, bool upperHalf )
    : track_( track ), clip_( clip ), grabEnd_( grabEnd ),
      toTime_( toTime ), upperHalf_( upperHalf )
{
}

SApplyResult SDragClipEdgeAction::apply( SProject * /*project*/ )
{
    // The arranger lives under the main window. In test mode the window is
    // built but never shown, which is fine — the gesture handlers work off the
    // view's own time/zoom mapping, not off screen geometry.
    SMainWindow *win = NULL;
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( ( win = qobject_cast<SMainWindow*>( w ) ) ) break;
    }
    if( !win ) {
        qWarning() << "SDragClipEdgeAction: no main window";
        return {false, nullptr};
    }
    if( !win->dragClipEdge( track_, clip_, grabEnd_, toTime_, upperHalf_ ) ) {
        qWarning() << "SDragClipEdgeAction: no clip at track" << track_
                   << "index" << clip_;
        return {false, nullptr};
    }
    // The drag submits its own SResizeClipAction on release; undoing that is
    // what reverses this gesture.
    return {true, nullptr};
}

void SDragClipEdgeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "track", track_ );
    elem.setAttribute( "clip", clip_ );
    elem.setAttribute( "edge", grabEnd_ ? "end" : "start" );
    elem.setAttribute( "toTime", QString::number( (qint64) toTime_ ) );
    elem.setAttribute( "half", upperHalf_ ? "upper" : "lower" );
}

bool SDragClipEdgeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    track_ = elem.attribute( "track", "0" ).toInt();
    clip_  = elem.attribute( "clip", "0" ).toInt();

    QString edge = elem.attribute( "edge", "end" );
    if( edge != "end" && edge != "start" ) {
        qWarning() << "SDragClipEdgeAction::readXml: unknown edge:" << edge;
        return false;
    }
    grabEnd_ = ( edge == "end" );

    QString half = elem.attribute( "half", "lower" );
    if( half != "lower" && half != "upper" ) {
        qWarning() << "SDragClipEdgeAction::readXml: unknown half:" << half;
        return false;
    }
    upperHalf_ = ( half == "upper" );

    bool ok = false;
    toTime_ = (offset_t) elem.attribute( "toTime", "0" ).toLongLong( &ok );
    if( !ok || toTime_ < 0 ) {
        qWarning() << "SDragClipEdgeAction::readXml: invalid toTime:"
                   << elem.attribute( "toTime" );
        return false;
    }
    return true;
}

static const bool s_reg_dragclipedge = (
    SActionRegistry::instance().registerType(
        QStringLiteral("drag-clip-edge"),
        []{ return new SDragClipEdgeAction; }
    ), true
);
