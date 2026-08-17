#include "app/eventui/seventeditorview.h"

#include <algorithm>

#include "app/eventui/seventtimeaxis.h"

#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidieventactions.h"
#include "app/shell/sapplication.h"

#include "tw/events/twtempomap.h"

SEventEditorView::SEventEditorView( QWidget *parent )
    : QWidget( parent )
{
}

SEventEditorView::~SEventEditorView() = default;

void SEventEditorView::setClip( const QList<int> &clipPath )
{
    clipPath_ = clipPath;
    refresh();
}

void SEventEditorView::setTimeAxis( SEventTimeAxis *axis )
{
    if( axis_ == axis ) return;
    if( axis_ ) QObject::disconnect( axis_, nullptr, this, nullptr );
    axis_ = axis;
    if( axis_ )
        QObject::connect( axis_, &SEventTimeAxis::axisChanged,
                          this, QOverload<>::of( &QWidget::update ) );
    update();
}

void SEventEditorView::setGrid( const QString &grid )
{
    if( grid_ == grid ) return;
    grid_ = grid;
    update();
}

void SEventEditorView::refresh()
{
    update();
}

QString SEventEditorView::describe() const
{
    const Resolved r = resolve();
    QString out = QStringLiteral( "kind=%1|notes=%2|grid=%3|linked=%4|empty=%5"
                                 "|selection=%6" )
        .arg( kind() )
        .arg( noteCount() )
        .arg( grid_ )
        .arg( ( axis_ && axis_->linked() ) ? 1 : 0 )
        .arg( r.valid() ? 0 : 1 )
        .arg( selectionCount() );
    const QString extra = describeExtra();
    if( !extra.isEmpty() ) out += QStringLiteral( "|" ) + extra;
    return out;
}

// ---------------------------------------------------------------------------
// Resolution — path in, nothing cached out (rule 1)
// ---------------------------------------------------------------------------

SEventEditorView::Resolved SEventEditorView::resolve() const
{
    Resolved r;
    if( clipPath_.isEmpty() ) return r;

    SProject *project = SApplication::app().getCurrentProject();
    if( !project ) return r;
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return r;

    SLink *link = splacements::placementAt( mixer, clipPath_ );
    if( !link ) return r;

    SObject *obj = &link->getSObject();
    // A take stack presents its ACTIVE lane through the generic take seam; a
    // stack is homogeneous by contentKind, so a MIDI stack yields a MIDI take
    // or nothing at all.
    if( SClipWindow *w = obj->windowTakeAt( -1 ) ) obj = &w->asObject();
    SMidiCut *cut = dynamic_cast<SMidiCut *>( obj );
    if( !cut ) return r;

    r.project = project;
    r.link    = link;
    r.cut     = cut;
    r.seq     = cut->sequence();
    return r;
}

// ---------------------------------------------------------------------------
// Ticks <-> frames <-> pixels (rule 3)
// ---------------------------------------------------------------------------

offset_t SEventEditorView::frameOfTick( const Resolved &r, qint64 tick ) const
{
    if( !r.valid() ) return 0;
    // The SAME expression SMidiCut::rebuild_nolock derives a note's frame
    // position with: (tick - anchor) * rate * framesPerTick, floored once.
    const twTempoMap &map = r.project->tempoMap();
    const Fraction framesPerTick =
        map.ticksToFrames( TickLen( (int64_t) 1 ), r.project->getSRate() );
    const Fraction rel = ( Fraction( (int64_t) tick ) - r.cut->getSrcStartTicks() )
                         * r.cut->getRateExact() * framesPerTick;
    const offset_t start = r.link->hasStartTime() ? r.link->getStartTime() : 0;
    return start + (offset_t) rel.floorToInt();
}

qint64 SEventEditorView::tickOfFrame( const Resolved &r, offset_t frame ) const
{
    if( !r.valid() ) return 0;
    const offset_t start = r.link->hasStartTime() ? r.link->getStartTime() : 0;
    const Fraction rel( (int64_t) ( frame - start ) );
    // The window's own inverse, so a round trip through the two lands where
    // resize-clip's slip arithmetic would put it.
    return (qint64) r.cut->timelineToSourceExact( rel ).floorToInt();
}

int SEventEditorView::xOfTick( const Resolved &r, qint64 tick ) const
{
    if( !axis_ ) return 0;
    return axis_->xOfFrame( frameOfTick( r, tick ) );
}

qint64 SEventEditorView::tickOfX( const Resolved &r, int x ) const
{
    if( !axis_ ) return 0;
    return tickOfFrame( r, axis_->frameOfX( x ) );
}

qint64 SEventEditorView::gridTicks( const Resolved &r ) const
{
    const int ppq = r.seq ? r.seq->ppq() : SMidiSequence::DEFAULT_PPQ;
    // The verb's own parser, so the editor's grid and `quantize-notes grid=`
    // can never disagree about what "1/8t" means.
    return SQuantizeNotesAction::gridTicks( grid_, ppq );
}

qint64 SEventEditorView::snapTick( const Resolved &r, qint64 tick ) const
{
    const qint64 g = gridTicks( r );
    if( g <= 0 ) return tick;
    if( tick < 0 ) tick = 0;
    return ( ( tick + g / 2 ) / g ) * g;
}

std::vector<SEvent> SEventEditorView::notesOf( const Resolved &r ) const
{
    std::vector<SEvent> out;
    if( !r.seq ) return out;
    for( const SEvent &e : r.seq->events() )
        if( e.kind == twEventKind::NoteOn ) out.push_back( e );
    std::sort( out.begin(), out.end() );
    return out;
}

void SEventEditorView::commitNotes( const Resolved &r,
                                    const std::vector<SEvent> &notes )
{
    if( !r.valid() ) return;
    // ONE action per gesture (rule 2). set-notes is absolute: the previous note
    // state IS the inverse, so undo is exact and the CCs underneath survive.
    SApplication::app().submitAction(
        new SSetNotesAction( clipPath_, notes, -1, true ) );
}

// ---------------------------------------------------------------------------
// The kind registry
// ---------------------------------------------------------------------------

SEventEditorRegistry &SEventEditorRegistry::instance()
{
    static SEventEditorRegistry reg;
    return reg;
}

void SEventEditorRegistry::registerKind( const QString &kind,
                                         const QString &label, Factory f )
{
    for( Entry &e : entries_ ) {
        if( e.kind == kind ) { e.label = label; e.factory = std::move( f ); return; }
    }
    entries_.push_back( Entry{ kind, label, std::move( f ) } );
}

QStringList SEventEditorRegistry::kinds() const
{
    QStringList out;
    for( const Entry &e : entries_ ) out << e.kind;
    return out;
}

QString SEventEditorRegistry::labelFor( const QString &kind ) const
{
    for( const Entry &e : entries_ ) if( e.kind == kind ) return e.label;
    return QString();
}

SEventEditorView *SEventEditorRegistry::create( const QString &kind,
                                                QWidget *parent ) const
{
    for( const Entry &e : entries_ )
        if( e.kind == kind && e.factory ) return e.factory( parent );
    return nullptr;
}
