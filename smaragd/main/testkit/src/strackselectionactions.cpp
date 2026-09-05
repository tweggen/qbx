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

// --- fader-key ---------------------------------------------------------------

SApplyResult SHeadFaderAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "head-fader: no main window";
        return { false, nullptr };
    }
    if( !win->setHeadFaderDb( trackPath_, db_ ) ) {
        qWarning() << "head-fader: no head for" << trackPath_
                   << "(a hidden lane has none), or the fader already holds"
                      " that tick";
        return { false, nullptr };
    }
    // NOT undoable itself: the fader's own handler submits the real
    // set-track-volume (or an automation write tick while playing), and THAT
    // is the undo step -- exactly as the automation UI gestures are.
    return { true, nullptr };
}

void SHeadFaderAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "db", QString::number( db_ ) );
}

bool SHeadFaderAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    db_        = elem.attribute( "db", "0" ).toDouble();
    return true;
}

static const bool s_reg_headfader = (
    SActionRegistry::instance().registerType(
        QStringLiteral("head-fader"),
        []{ return new SHeadFaderAction; }
    ), true
);

SApplyResult SFaderKeyAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) {
        qWarning() << "fader-key: no main window";
        return { false, nullptr };
    }
    if( !win->sendFaderKey( trackPath_, key_ ) ) {
        qWarning() << "fader-key: no head for" << trackPath_
                   << "or unknown key" << key_;
        return { false, nullptr };
    }
    // The keystroke reaches the transport (a manual seek); nothing here to
    // undo, exactly like select-track.
    return { true, nullptr };
}

void SFaderKeyAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "key", key_ );
}

bool SFaderKeyAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    key_       = elem.attribute( "key", "Home" );
    if( key_ != QStringLiteral( "Home" ) && key_ != QStringLiteral( "End" ) ) {
        qWarning() << "fader-key: unknown key:" << key_;
        return false;
    }
    return true;
}

// --- assert-track-volume ------------------------------------------------------

SApplyResult SAssertTrackVolumeAction::apply( SProject *project )
{
    // The path text may carry its own root qualifier ("Drums:0"); an
    // explicit pathRoot_ is the fallback for the bare spelling.
    const strackpath::QualifiedPath q_ = strackpath::parseQualified( trackPath_ );
    SObject *root = splacements::rootNamed(
        project, q_.root.isEmpty() ? pathRoot_ : q_.root );
    SObject *lane = splacements::laneAt( root, q_.idx );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) {
        qWarning() << "assert-track-volume: no track at" << trackPath_;
        return { false, nullptr };
    }
    const double actual = track->getVolume();
    if( qAbs( actual - volumeDb_ ) > tolerance_ ) {
        qWarning() << "assert-track-volume FAILED: expected" << volumeDb_
                   << "dB but got" << actual << "dB (tolerance" << tolerance_ << ")";
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertTrackVolumeAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "volumeDb", QString::number( volumeDb_ ) );
    elem.setAttribute( "tolerance", QString::number( tolerance_ ) );
}

bool SAssertTrackVolumeAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    volumeDb_  = elem.attribute( "volumeDb", "0" ).toDouble();
    tolerance_ = elem.attribute( "tolerance", "0.001" ).toDouble();
    return true;
}

// --- assert-track-selection -------------------------------------------------

SApplyResult SAssertTrackSelectionAction::apply( SProject *project )
{
    SObject *root = splacements::rootNamed( project, pathRoot_ );
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

static const bool s_reg_faderkey =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "fader-key" ),
          [] { return new SFaderKeyAction; } ),
      true );

static const bool s_reg_asserttrackvolume =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-track-volume" ),
          [] { return new SAssertTrackVolumeAction; } ),
      true );

static const bool s_reg_asserttrackselection =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-track-selection" ),
          [] { return new SAssertTrackSelectionAction; } ),
      true );

// --- assert-track-name -------------------------------------------------

static STrack *trackAt( SProject *project, const QString &pathRoot_,
                        const QString &path )
{
    // The path text may carry its own root qualifier ("Drums:0"); an
    // explicit pathRoot_ is the fallback for the bare spelling.
    const strackpath::QualifiedPath q_ = strackpath::parseQualified( path );
    SObject *root = splacements::rootNamed(
        project, q_.root.isEmpty() ? pathRoot_ : q_.root );
    SObject *lane = splacements::laneAt( root, q_.idx );
    return dynamic_cast<STrack *>( lane );
}

SApplyResult SAssertTrackNameAction::apply( SProject *project )
{
    STrack *track = trackAt( project, pathRoot_, trackPath_ );
    if( !track ) {
        qWarning() << "assert-track-name: no track at" << trackPath_;
        return { false, nullptr };
    }
    const QString actual = track->getSName();

    if( !name_.isEmpty() && actual != name_ ) {
        qWarning() << "assert-track-name FAILED: expected" << name_
                   << "but got" << actual;
        return { false, nullptr };
    }
    if( !prefix_.isEmpty() && !actual.startsWith( prefix_ ) ) {
        qWarning() << "assert-track-name FAILED: expected prefix" << prefix_
                   << "but got" << actual;
        return { false, nullptr };
    }
    if( !suffix_.isEmpty() && !actual.endsWith( suffix_ ) ) {
        qWarning() << "assert-track-name FAILED: expected suffix" << suffix_
                   << "but got" << actual;
        return { false, nullptr };
    }
    if( !differsFrom_.isEmpty() ) {
        STrack *other = trackAt( project, pathRoot_, differsFrom_ );
        if( !other ) {
            qWarning() << "assert-track-name: no track at" << differsFrom_
                       << "(differsFrom)";
            return { false, nullptr };
        }
        if( actual == other->getSName() ) {
            qWarning() << "assert-track-name FAILED:" << trackPath_ << "and"
                       << differsFrom_ << "have the same name" << actual;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SAssertTrackNameAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    if( !name_.isEmpty() )        elem.setAttribute( "name", name_ );
    if( !prefix_.isEmpty() )      elem.setAttribute( "prefix", prefix_ );
    if( !suffix_.isEmpty() )      elem.setAttribute( "suffix", suffix_ );
    if( !differsFrom_.isEmpty() ) elem.setAttribute( "differsFrom", differsFrom_ );
}

bool SAssertTrackNameAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_   = elem.attribute( "trackPath", "0" );
    name_        = elem.attribute( "name", "" );
    prefix_      = elem.attribute( "prefix", "" );
    suffix_      = elem.attribute( "suffix", "" );
    differsFrom_ = elem.attribute( "differsFrom", "" );
    return true;
}

static const bool s_reg_asserttrackname =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "assert-track-name" ),
          [] { return new SAssertTrackNameAction; } ),
      true );
