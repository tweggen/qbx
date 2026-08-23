#include "app/eventui/seventeditordock.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>
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

    auto mkTool = [&]( const QString &text, const QString &tip ) {
        QToolButton *b = new QToolButton( bar );
        b->setText( text );
        b->setToolTip( tip );
        b->setCheckable( true );
        b->setAutoExclusive( true );
        barLayout->addWidget( b );
        connect( b, &QToolButton::toggled, this,
                 &SEventEditorDock::onToolChanged );
        return b;
    };
    toolSelect_ = mkTool( tr( "S" ), tr( "Select / move / resize notes" ) );
    toolDraw_   = mkTool( tr( "D" ), tr( "Draw notes" ) );
    toolErase_  = mkTool( tr( "E" ), tr( "Erase notes" ) );
    toolSelect_->setChecked( true );

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
    ruler_->setLeftInset( 40 );   // the piano roll's keyboard column
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
    view_->setTool( toolDraw_ && toolDraw_->isChecked()
                        ? SEventEditorView::Tool::Draw
                    : toolErase_ && toolErase_->isChecked()
                        ? SEventEditorView::Tool::Erase
                        : SEventEditorView::Tool::Select );
    viewSlot_->addWidget( view_, 1 );
    view_->setClip( clipPath_ );
    placeholder_->setVisible( clipPath_.isEmpty() );
    view_->setVisible( !clipPath_.isEmpty() );

    if( kindCombo_ ) {
        const QSignalBlocker block( kindCombo_ );
        const int idx = kindCombo_->findData( kind );
        if( idx >= 0 ) kindCombo_->setCurrentIndex( idx );
    }
}

QString SEventEditorDock::describe() const
{
    if( view_ ) return view_->describe();
    // No registered kind at all: report the same shape a bound view would, so
    // a case can always match on `empty=`.
    return QStringLiteral( "kind=|notes=0|grid=%1|linked=%2|empty=1|selection=0" )
        .arg( gridCombo_ ? gridCombo_->currentText() : QStringLiteral( "1/16" ) )
        .arg( ( axis_ && axis_->linked() ) ? 1 : 0 );
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

    updating_ = false;
}

void SEventEditorDock::showEvent( QShowEvent *event )
{
    QWidget::showEvent( event );
    refresh();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void SEventEditorDock::onToolChanged()
{
    if( !view_ ) return;
    view_->setTool( toolDraw_->isChecked()  ? SEventEditorView::Tool::Draw
                  : toolErase_->isChecked() ? SEventEditorView::Tool::Erase
                                            : SEventEditorView::Tool::Select );
}

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
