#include "app/testkit/strackselectionactions.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"
#include "app/model/sproject.h"
#include "app/model/splacements.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"
#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QStringList>

// The arranger lives under the main window, and testkit may not include
// app/timeline (tools/check_layering.py) — so the gesture verbs reach it
// through the shell, the same route drag-clip-edge uses. The ASSERTION does
// not: the selection set lives on SStdMixer, which testkit may include.
static SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

static bool parseModifiers( const QString &modStr, Qt::KeyboardModifiers &out,
                            const char *who )
{
    out = Qt::NoModifier;
    const QString s = modStr.trimmed();
    if( s.isEmpty() ) return true;
    const QStringList parts = s.split( '+', Qt::SkipEmptyParts );
    for( const QString &raw : parts ) {
        const QString m = raw.trimmed().toLower();
        if( m == "ctrl" )       out |= Qt::ControlModifier;
        else if( m == "shift" ) out |= Qt::ShiftModifier;
        else if( m == "alt" )   out |= Qt::AltModifier;
        else {
            qWarning() << who << ": unknown modifier:" << raw;
            return false;
        }
    }
    return true;
}

// --- select-track -----------------------------------------------------------

SApplyResult SSelectTrackAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "select-track: no main window";
        return { false, nullptr };
    }
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    if( !parseModifiers( modifiers_, mods, "select-track" ) ) {
        return { false, nullptr };
    }
    if( !win->selectTrackGesture( trackPath_, mods ) ) {
        qWarning() << "select-track: no track at" << trackPath_;
        return { false, nullptr };
    }
    return { true, nullptr };   // selection is view state: nothing to undo
}

void SSelectTrackAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    if( !modifiers_.isEmpty() ) elem.setAttribute( "modifiers", modifiers_ );
}

bool SSelectTrackAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    modifiers_ = elem.attribute( "modifiers", "" );
    Qt::KeyboardModifiers ignored;
    return parseModifiers( modifiers_, ignored, "select-track" );
}

// --- track-head-toggle ------------------------------------------------------

SApplyResult STrackHeadToggleAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "track-head-toggle: no main window";
        return { false, nullptr };
    }
    if( !win->toggleTrackHead( trackPath_, control_, on_ ) ) {
        qWarning() << "track-head-toggle: no head for" << trackPath_
                   << "or unknown control" << control_;
        return { false, nullptr };
    }
    // The button pushed whatever actions it pushes (a macro for a broadcast);
    // undoing THAT reverses the gesture. Nothing to invert here.
    return { true, nullptr };
}

void STrackHeadToggleAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "control", control_ );
    elem.setAttribute( "on", on_ ? "1" : "0" );
}

bool STrackHeadToggleAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    control_   = elem.attribute( "control", "mute" ).trimmed().toLower();
    static const QStringList known{ "mute", "solo", "arm", "takes", "group" };
    if( !known.contains( control_ ) ) {
        qWarning() << "track-head-toggle: unknown control:" << control_;
        return false;
    }
    const QString v = elem.attribute( "on", "1" );
    on_ = ( v == "1" || v.compare( "true", Qt::CaseInsensitive ) == 0 );
    return true;
}

// --- drag-track -------------------------------------------------------------

SApplyResult SDragTrackAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "drag-track: no main window";
        return { false, nullptr };
    }
    if( !win->dragTrackHead( trackPath_, targetRow_, nestOnto_ ) ) {
        qWarning() << "drag-track: no head for" << trackPath_
                   << "or bad targetRow" << targetRow_;
        return { false, nullptr };
    }
    // The drop pushed its own action(s) — a macro when several tracks moved.
    return { true, nullptr };
}

void SDragTrackAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "targetRow", targetRow_ );
    elem.setAttribute( "mode", nestOnto_ ? "onto" : "before" );
}

bool SDragTrackAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    bool ok = false;
    targetRow_ = elem.attribute( "targetRow", "0" ).toInt( &ok );
    if( !ok || targetRow_ < 0 ) {
        qWarning() << "drag-track: invalid targetRow:"
                   << elem.attribute( "targetRow" );
        return false;
    }
    const QString mode = elem.attribute( "mode", "onto" ).trimmed().toLower();
    if( mode == "onto" )        nestOnto_ = true;
    else if( mode == "before" ) nestOnto_ = false;
    else {
        qWarning() << "drag-track: unknown mode:" << mode;
        return false;
    }
    return true;
}

// --- assert-track-selection -------------------------------------------------

SApplyResult SAssertTrackSelectionAction::apply( SProject *project )
{
    SObject *root = splacements::rootContainer( project );
    SStdMixer *mixer = dynamic_cast<SStdMixer *>( root );
    if( !mixer ) {
        qWarning() << "assert-track-selection: no root mixer";
        return { false, nullptr };
    }

    QStringList have;
    for( STrack *t : mixer->getSelectedTracks() ) {
        const QList<int> p = strackpath::pathOf( mixer, t );
        // A selected track that is no longer IN the tree resolves to the empty
        // path; report it as such rather than silently dropping it.
        have << ( p.isEmpty() ? QStringLiteral( "<detached>" )
                              : strackpath::pathToString( p ) );
    }
    QStringList want = paths_.split( ';', Qt::SkipEmptyParts );
    for( QString &w : want ) w = w.trimmed();

    // Compared as a SET: click order is not part of the contract.
    QStringList haveSorted = have;  haveSorted.sort();
    QStringList wantSorted = want;  wantSorted.sort();
    if( haveSorted != wantSorted ) {
        qWarning() << "assert-track-selection FAILED: expected"
                   << wantSorted << "but the selection is" << haveSorted;
        return { false, nullptr };
    }

    if( hasPrimary_ ) {
        STrack *p = mixer->getSelectedTrack();
        const QString actual =
            p ? strackpath::pathToString( strackpath::pathOf( mixer, p ) )
              : QString();
        if( actual != primary_.trimmed() ) {
            qWarning() << "assert-track-selection FAILED: expected primary"
                       << primary_ << "but it is" << actual;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SAssertTrackSelectionAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "paths", paths_ );
    if( hasPrimary_ ) elem.setAttribute( "primary", primary_ );
}

bool SAssertTrackSelectionAction::readXml( const QDomElement &elem, int /*version*/ )
{
    paths_      = elem.attribute( "paths", "" );
    hasPrimary_ = elem.hasAttribute( "primary" );
    primary_    = elem.attribute( "primary", "" );
    return true;
}

static const bool s_reg_selecttrack =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "select-track" ),
          [] { return new SSelectTrackAction; } ),
      true );

static const bool s_reg_trackheadtoggle =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "track-head-toggle" ),
          [] { return new STrackHeadToggleAction; } ),
      true );

static const bool s_reg_dragtrack =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "drag-track" ),
          [] { return new SDragTrackAction; } ),
      true );

static const bool s_reg_asserttrackselection =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-track-selection" ),
          [] { return new SAssertTrackSelectionAction; } ),
      true );
