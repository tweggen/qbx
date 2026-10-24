#include "app/eventui/seventeditordock.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "app/eventui/seventeditorview.h"
#include "app/eventui/seventtimeaxis.h"
#include "app/eventui/seventtimeruler.h"
#include "app/eventui/spianorollview.h"

#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobject.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidieventactions.h"
#include "app/shell/sapplication.h"

namespace {

// The divisions the toolbar offers. Straight/triplet pairs down to 1/32 -
// SQuantizeNotesAction::gridTicks() is the ONE parser for these strings, so the
// editor's grid, its snapping and `quantize-notes grid=` can never disagree.
const char *const kGrids[] = {
    "1/1", "1/2", "1/4", "1/4t", "1/8", "1/8t", "1/16", "1/16t", "1/32", "1/32t"
};

// The keyboard column the ruler and the horizontal scroll bar are inset by,
// so both line up with the note grid.
constexpr int kViewInset = 40;

// A short list of controllers worth a lane by default.
struct CcEntry { int cc; const char *name; };
const CcEntry kCcs[] = {
    {   1, "1 Modulation" }, {   7, "7 Volume" },  {  10, "10 Pan" },
    {  11, "11 Expression" }, {  64, "64 Sustain" }, {  74, "74 Cutoff" }
};

}  // namespace

SEventEditorDock::SEventEditorDock( QWidget *parent )
    : QWidget( parent )
{
    axis_ = new SEventTimeAxis( this );
    buildUi();
    // The first registered kind is the default, which is the piano roll.
    const QStringList kinds = SEventEditorRegistry::instance().kinds();
    if( !kinds.isEmpty() ) setKind( kinds.first() );
    refresh();
}

SEventEditorDock::~SEventEditorDock() = default;

SPianoRollView *SEventEditorDock::pianoRoll() const
{
    return dynamic_cast<SPianoRollView *>( view_ );
}

void SEventEditorDock::buildUi()
{
    QVBoxLayout *outer = new QVBoxLayout( this );
    outer->setContentsMargins( 0, 0, 0, 0 );
    outer->setSpacing( 0 );

    // --- toolbar ----------------------------------------------------------
    QWidget *bar = new QWidget( this );
    QHBoxLayout *barLayout = new QHBoxLayout( bar );
    barLayout->setContentsMargins( 4, 2, 4, 2 );
    barLayout->setSpacing( 4 );

    // No tool buttons (QBX-221): the editor has no Select/Draw/Erase modes.
    // A double-click on empty grid creates a note and Delete/Backspace
    // removes the selected ones, whatever was clicked last.
    barLayout->addWidget( new QLabel( tr( "Grid:" ), bar ) );
    gridCombo_ = new QComboBox( bar );
    for( const char *g : kGrids ) gridCombo_->addItem( QLatin1String( g ) );
    gridCombo_->setCurrentText( QStringLiteral( "1/16" ) );
    barLayout->addWidget( gridCombo_ );
    connect( gridCombo_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &SEventEditorDock::onGridChanged );

    quantize_ = new QPushButton( tr( "Quantize" ), bar );
    quantize_->setToolTip( tr( "Snap every note in this clip to the grid "
                               "(one undo step) — Q" ) );
    barLayout->addWidget( quantize_ );
    connect( quantize_, &QPushButton::clicked, this,
             &SEventEditorDock::onQuantizeClicked );

    // "Q" = quantize, discoverable from the button's own tooltip above.
    // WidgetWithChildrenShortcut scopes it to this dock (and its children,
    // including the piano roll view itself) so it fires only while the event
    // editor has focus — a window-wide shortcut would collide with the
    // virtual keyboard's OWN Q = note-12 binding
    // (svirtualkeyboarddock.cpp:36), which needs bare Q for playing while
    // that dock has focus instead. SPianoRollView::keyPressEvent's
    // `default: ignore()` is what lets an unhandled key bubble up here from
    // the view.
    actQuantize_ = new QAction( tr( "Quantize" ), this );
    actQuantize_->setShortcut( QKeySequence( Qt::Key_Q ) );
    actQuantize_->setShortcutContext( Qt::WidgetWithChildrenShortcut );
    connect( actQuantize_, &QAction::triggered,
             this, &SEventEditorDock::onQuantizeClicked );
    addAction( actQuantize_ );

    barLayout->addWidget( new QLabel( tr( "CC:" ), bar ) );
    ccCombo_ = new QComboBox( bar );
    for( const CcEntry &e : kCcs )
        ccCombo_->addItem( QLatin1String( e.name ), e.cc );
    barLayout->addWidget( ccCombo_ );
    ccButton_ = new QPushButton( tr( "Lane" ), bar );
    ccButton_->setToolTip( tr( "Show or hide a controller lane" ) );
    barLayout->addWidget( ccButton_ );
    connect( ccButton_, &QPushButton::clicked, this,
             &SEventEditorDock::onCcLaneClicked );

    barLayout->addStretch( 1 );

    linkCheck_ = new QCheckBox( tr( "Link" ), bar );
    linkCheck_->setToolTip( tr( "Follow the arranger's zoom and scroll" ) );
    linkCheck_->setChecked( true );
    barLayout->addWidget( linkCheck_ );
    connect( linkCheck_, &QCheckBox::toggled, this,
             &SEventEditorDock::onLinkToggled );

    kindCombo_ = new QComboBox( bar );
    for( const QString &k : SEventEditorRegistry::instance().kinds() ) {
        const QString label = SEventEditorRegistry::instance().labelFor( k );
        kindCombo_->addItem( label.isEmpty() ? k : label, k );
    }
    barLayout->addWidget( kindCombo_ );
    connect( kindCombo_, QOverload<int>::of( &QComboBox::currentIndexChanged ),
             this, &SEventEditorDock::onKindChanged );

    outer->addWidget( bar );

    // --- ruler ------------------------------------------------------------
    ruler_ = new SEventTimeRuler( this );
    ruler_->setTimeAxis( axis_ );
    ruler_->setLeftInset( kViewInset );   // the piano roll's keyboard column
    outer->addWidget( ruler_ );

    // --- the view slot ----------------------------------------------------
    QWidget *slot = new QWidget( this );
    viewSlot_ = new QVBoxLayout( slot );
    viewSlot_->setContentsMargins( 0, 0, 0, 0 );
    viewSlot_->setSpacing( 0 );
    placeholder_ = new QLabel( tr( "No event clip selected" ), slot );
    placeholder_->setAlignment( Qt::AlignCenter );
    placeholder_->setEnabled( false );
    viewSlot_->addWidget( placeholder_ );
    outer->addWidget( slot, 1 );

    // --- the horizontal scroll bar (QBX-216) ------------------------------
    // Over the SHARED axis, so it moves the ruler, every band and (when the
    // view is linked) is overridden by the arranger's next scroll exactly as
    // a Shift+wheel pan is. Inset by the keyboard column so it lines up with
    // the grid it scrolls.
    QWidget *hRow = new QWidget( this );
    QHBoxLayout *hLayout = new QHBoxLayout( hRow );
    hLayout->setContentsMargins( kViewInset, 0, 0, 0 );
    hLayout->setSpacing( 0 );
    hScroll_ = new QScrollBar( Qt::Horizontal, hRow );
    hLayout->addWidget( hScroll_ );
    outer->addWidget( hRow );
    connect( hScroll_, &QScrollBar::valueChanged, this,
             [this]( int v ) { if( axis_ ) axis_->setLeftPixels( v ); } );
    connect( axis_, &SEventTimeAxis::axisChanged,
             this, &SEventEditorDock::syncHScroll );
}

int SEventEditorDock::timeViewportWidth() const
{
    if( view_ ) return std::max( 1, view_->timeRight() - view_->timeLeft() );
    return std::max( 1, width() - kViewInset );
}

void SEventEditorDock::syncHScroll()
{
    if( !hScroll_ || !axis_ ) return;
    offset_t cs = 0, ce = 0;
    const bool bound = view_ && view_->clipFrames( &cs, &ce );
    hScroll_->setVisible( bound );

    // The range is in the axis's own ABSOLUTE pixels (leftPixels' unit). It
    // covers the clip's end plus a quarter page of room, and never less than
    // where the axis already is: a linked arranger may have scrolled further
    // than the clip reaches, and a range that excluded it would make the bar
    // jump - and push the axis back - the moment it is touched.
    const int vp   = timeViewportWidth();
    const int left = axis_->leftPixels();
    const int endX = bound ? axis_->xOfFrame( ce ) + left + vp / 4 : 0;
    const int maxV = std::max( { 0, endX - vp, left } );

    const QSignalBlocker block( hScroll_ );   // a SYNC, not a gesture
    hScroll_->setRange( 0, maxV );
    hScroll_->setPageStep( vp );
    hScroll_->setSingleStep( std::max( 1, vp / 10 ) );
    hScroll_->setValue( left );
}

bool SEventEditorDock::tkScrollTime( const QString &to )
{
    if( !view_ || !hScroll_ ) return false;
    // A never-shown dock has whatever size the layout last gave its view;
    // give it a usable one so the page is somewhere real (tkDragNote's trick).
    if( view_->width() < 640 || view_->height() < 360 )
        view_->resize( std::max( view_->width(), 640 ),
                       std::max( view_->height(), 360 ) );
    syncHScroll();
    if( hScroll_->isHidden() ) return false;   // nothing bound

    // The REAL scroll bar, the same call a dragged handle makes, so its
    // valueChanged connection is what moves the axis.
    if(      to == QLatin1String( "start" ) ) hScroll_->setValue( hScroll_->minimum() );
    else if( to == QLatin1String( "end" ) )   hScroll_->setValue( hScroll_->maximum() );
    else {
        bool ok = false;
        const int v = to.toInt( &ok );
        if( !ok ) return false;
        hScroll_->setValue( v );
    }
    return true;
}

void SEventEditorDock::resizeEvent( QResizeEvent *event )
{
    QWidget::resizeEvent( event );
    syncHScroll();
}

void SEventEditorDock::setKind( const QString &kind )
{
    if( kind_ == kind && view_ ) return;
    SEventEditorView *next =
        SEventEditorRegistry::instance().create( kind, nullptr );
    if( !next ) return;

    if( view_ ) {
        viewSlot_->removeWidget( view_ );
        view_->deleteLater();
        view_ = nullptr;
    }
    kind_ = kind;
    view_ = next;
    view_->setTimeAxis( axis_ );
    view_->setGrid( gridCombo_ ? gridCombo_->currentText()
                               : QStringLiteral( "1/16" ) );
    viewSlot_->addWidget( view_, 1 );
    view_->setClip( clipPath_ );
    placeholder_->setVisible( clipPath_.isEmpty() );
    view_->setVisible( !clipPath_.isEmpty() );

    if( kindCombo_ ) {
        const QSignalBlocker block( kindCombo_ );
        const int idx = kindCombo_->findData( kind );
        if( idx >= 0 ) kindCombo_->setCurrentIndex( idx );
    }
    syncHScroll();
}

QString SEventEditorDock::describe() const
{
    if( view_ ) return view_->describe() + describeHScroll();
    // No registered kind at all: report the same shape a bound view would, so
    // a case can always match on `empty=`.
    return QStringLiteral( "kind=|notes=0|grid=%1|linked=%2|empty=1|selection=0" )
        .arg( gridCombo_ ? gridCombo_->currentText() : QStringLiteral( "1/16" ) )
        .arg( ( axis_ && axis_->linked() ) ? 1 : 0 );
}

QString SEventEditorDock::describeHScroll() const
{
    // APPENDED after the view's own fields (the order is the contract), and
    // all of them platform-independent truths rather than pixel counts: a
    // scroll bar's width differs by style, so a pixel would only hold on one.
    if( !hScroll_ || !axis_ || !view_ ) return QString();
    offset_t cs = 0, ce = 0;
    const bool bound = view_->clipFrames( &cs, &ce );
    const int vp = timeViewportWidth();
    auto onScreen = [&]( offset_t f ) {
        const int x = axis_->xOfFrame( f );
        return bound && x >= 0 && x <= vp;
    };
    return QStringLiteral( "|hBar=%1|hSync=%2|atStart=%3|atEnd=%4"
                           "|clipStartVisible=%5|clipEndVisible=%6" )
        .arg( hScroll_->isHidden() ? 0 : 1 )
        .arg( hScroll_->value() == axis_->leftPixels() ? 1 : 0 )
        .arg( hScroll_->value() == hScroll_->minimum() ? 1 : 0 )
        .arg( hScroll_->value() == hScroll_->maximum() ? 1 : 0 )
        .arg( onScreen( cs ) ? 1 : 0 )
        .arg( onScreen( ce ) ? 1 : 0 );
}

// ---------------------------------------------------------------------------
// The selection follower (timeline invariants 8 and 9)
// ---------------------------------------------------------------------------

QList<int> SEventEditorDock::resolveSelectedEventClip() const
{
    SApplication &app = SApplication::app();
    SProject *project = app.getCurrentProject();
    if( !project ) return QList<int>();
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return QList<int>();

    // PATHS, never cached links. The first EVENT clip in the selection wins;
    // an audio-only selection resolves to nothing, which is what clears the
    // editor rather than leaving a stale clip on screen.
    for( const QList<int> &p : app.getCurrentSelectionPaths() ) {
        SLink *link = splacements::placementAt( mixer, p );
        if( !link ) continue;
        SObject *obj = &link->getSObject();
        if( SClipWindow *w = obj->windowTakeAt( -1 ) ) obj = &w->asObject();
        if( obj->contentKind() != SContentKind::Event ) continue;
        return p;
    }
    return QList<int>();
}

void SEventEditorDock::bindClip( const QList<int> &clipPath )
{
    if( clipPath.isEmpty() ) { refresh(); return; }
    clipPath_ = clipPath;
    if( view_ ) {
        view_->setClip( clipPath );
        view_->refresh();
        view_->setVisible( true );
    }
    // The placeholder and the view are exclusive: leaving both up paints
    // "No event clip selected" over a bound piano roll, which is exactly what
    // the first PNG grab showed.
    if( placeholder_ ) placeholder_->setVisible( false );
    if( ruler_ ) ruler_->update();
    syncHScroll();
}

void SEventEditorDock::refresh()
{
    if( updating_ ) return;
    updating_ = true;

    const QList<int> path = resolveSelectedEventClip();
    clipPath_ = path;
    if( view_ ) {
        view_->setClip( path );
        view_->refresh();
        view_->setVisible( !path.isEmpty() );
    }
    if( placeholder_ ) placeholder_->setVisible( path.isEmpty() );
    if( ruler_ ) ruler_->update();
    syncHScroll();

    updating_ = false;
}

void SEventEditorDock::showEvent( QShowEvent *event )
{
    QWidget::showEvent( event );
    refresh();
}

// ---------------------------------------------------------------------------
// Delete / Backspace belong to the editor while it has the focus (QBX-221)
// ---------------------------------------------------------------------------

namespace {

bool isDeleteKey( const QKeyEvent *ke )
{
    // Bare keys only, like the arranger's binding: Shift is let through
    // because a Shift+Delete still means "delete" to a user, Ctrl/Alt/Meta
    // are left to whatever else may bind them.
    const Qt::KeyboardModifiers held = ke->modifiers()
        & ( Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier );
    return held == Qt::NoModifier
        && ( ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace );
}

}  // namespace

bool SEventEditorDock::event( QEvent *ev )
{
    // The arranger's "Remove sample" binds Delete and Backspace WINDOW-wide,
    // and Qt fires a window shortcut before any keyPressEvent unless the
    // focus widget, or an ancestor on the way up, accepts the
    // ShortcutOverride. Nothing did, so Delete pressed in the editor removed
    // the clip being edited. Accepting it HERE covers every child of the
    // dock that can hold the focus (the piano roll, the toolbar combos, the
    // Link box), which is the ticket's "if the focus is inside the event
    // editor". A child that wants the key itself (a line edit) accepts it
    // first and keeps it.
    if( ev->type() == QEvent::ShortcutOverride
        && isDeleteKey( static_cast<QKeyEvent *>( ev ) ) ) {
        ev->accept();
        return true;
    }
    return QWidget::event( ev );
}

void SEventEditorDock::keyPressEvent( QKeyEvent *ev )
{
    // Reached from a child that did not take the key (the view consumes
    // Delete/Backspace itself), so hand it to the view: with the focus on the
    // grid combo, Delete still deletes notes. `forwardingKey_` stops a view
    // that ever ignored it from bouncing it straight back here for good.
    if( isDeleteKey( ev ) ) {
        if( view_ && !forwardingKey_ ) {
            forwardingKey_ = true;
            QApplication::sendEvent( view_, ev );
            forwardingKey_ = false;
        }
        ev->accept();
        return;
    }
    QWidget::keyPressEvent( ev );
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void SEventEditorDock::applyGridToChildren()
{
    const QString g = gridCombo_->currentText();
    if( view_ )  view_->setGrid( g );
    if( ruler_ ) ruler_->setGrid( g );
}

void SEventEditorDock::onGridChanged( int )
{
    applyGridToChildren();
}

void SEventEditorDock::onKindChanged( int index )
{
    if( index < 0 || !kindCombo_ ) return;
    setKind( kindCombo_->itemData( index ).toString() );
}

void SEventEditorDock::onLinkToggled( bool on )
{
    if( axis_ ) axis_->setLinked( on );
}

void SEventEditorDock::onQuantizeClicked()
{
    if( clipPath_.isEmpty() ) return;
    // A quantize is a set-notes COMPOSITION, so its inverse is the previous
    // state like every other content edit - one undo step for one click.
    SApplication::app().submitAction(
        new SQuantizeNotesAction( clipPath_, gridCombo_->currentText(),
                                  1.0, 0.0, -1, true ) );
}

void SEventEditorDock::onCcLaneClicked()
{
    SPianoRollView *roll = pianoRoll();
    if( !roll || !ccCombo_ ) return;
    const int cc = ccCombo_->currentData().toInt();
    if( roll->ccLanes().contains( cc ) ) roll->removeCcLane( cc );
    else                                 roll->addCcLane( cc );
}
