#include "app/eventui/svirtualkeyboarddock.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSpinBox>
#include <QToolButton>

#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidieventactions.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/shell/sapplication.h"

#include "tw/core/twfraction.h"

namespace {

// REAPER's virtual-keyboard map. Index = semitone above the base octave.
struct KeyMap { int qtKey; int semitone; };
const KeyMap kMap[] = {
    // lower octave: Z S X D C V G B H N J M
    { Qt::Key_Z, 0 }, { Qt::Key_S, 1 }, { Qt::Key_X, 2 }, { Qt::Key_D, 3 },
    { Qt::Key_C, 4 }, { Qt::Key_V, 5 }, { Qt::Key_G, 6 }, { Qt::Key_B, 7 },
    { Qt::Key_H, 8 }, { Qt::Key_N, 9 }, { Qt::Key_J, 10 }, { Qt::Key_M, 11 },
    // upper octave: Q 2 W 3 E R 5 T 6 Y 7 U
    { Qt::Key_Q, 12 }, { Qt::Key_2, 13 }, { Qt::Key_W, 14 }, { Qt::Key_3, 15 },
    { Qt::Key_E, 16 }, { Qt::Key_R, 17 }, { Qt::Key_5, 18 }, { Qt::Key_T, 19 },
    { Qt::Key_6, 20 }, { Qt::Key_Y, 21 }, { Qt::Key_7, 22 }, { Qt::Key_U, 23 }
};

bool isBlack( int semitone )
{
    switch( ( ( semitone % 12 ) + 12 ) % 12 ) {
    case 1: case 3: case 6: case 8: case 10: return true;
    default: return false;
    }
}

// The white-key index of a semitone within its octave (0..6), or -1 for black.
int whiteIndex( int semitone )
{
    static const int idx[ 12 ] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
    return idx[ ( ( semitone % 12 ) + 12 ) % 12 ];
}

/**
 * The clip a virtual key writes into: the first EVENT clip in the SELECTION,
 * else the first event clip on the SELECTED TRACK. An index path, resolved
 * fresh - the keyboard caches nothing, for the same reason every other
 * selection follower does not (timeline invariant 8).
 */
QList<int> targetEventClip()
{
    SApplication &app = SApplication::app();
    SProject *project = app.getCurrentProject();
    if( !project ) return QList<int>();
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return QList<int>();

    for( const QList<int> &p : app.getCurrentSelectionPaths() ) {
        SLink *link = splacements::placementAt( mixer, p );
        if( !link ) continue;
        SObject *obj = &link->getSObject();
        if( SClipWindow *w = obj->windowTakeAt( -1 ) ) obj = &w->asObject();
        if( obj->contentKind() == SContentKind::Event ) return p;
    }

    SStdMixer *stdMixer = dynamic_cast<SStdMixer *>( mixer );
    STrack *track = stdMixer ? stdMixer->getSelectedTrack() : nullptr;
    if( !track ) return QList<int>();
    const QList<int> trackPath = strackpath::pathOf( mixer, track );
    int i = 0;
    for( SLink *lk : track->childLinks() ) {
        SObject *obj = &lk->getSObject();
        if( SClipWindow *w = obj->windowTakeAt( -1 ) ) obj = &w->asObject();
        if( obj->contentKind() == SContentKind::Event ) {
            QList<int> out = trackPath;
            out.append( i );
            return out;
        }
        ++i;
    }
    return QList<int>();
}

}  // namespace

SVirtualKeyboardDock::SVirtualKeyboardDock( QWidget *parent )
    : QWidget( parent )
{
    setFocusPolicy( Qt::StrongFocus );
    setMinimumHeight( CONTROLS_H + 48 );
    buildUi();
}

SVirtualKeyboardDock::~SVirtualKeyboardDock() = default;

void SVirtualKeyboardDock::buildUi()
{
    // A thin control row across the top; the rest of the widget is the painted
    // keyboard, which is why the row is laid out with a bottom stretch rather
    // than filling the widget.
    QHBoxLayout *row = new QHBoxLayout;
    row->setContentsMargins( 4, 2, 4, 2 );
    row->setSpacing( 4 );

    octaveDown_ = new QToolButton( this );
    octaveDown_->setText( QStringLiteral( "-" ) );
    octaveDown_->setToolTip( tr( "Octave down" ) );
    row->addWidget( octaveDown_ );
    connect( octaveDown_, &QToolButton::clicked,
             this, [this] { setOctave( octave_ - 1 ); } );

    octaveLabel_ = new QLabel( this );
    row->addWidget( octaveLabel_ );

    octaveUp_ = new QToolButton( this );
    octaveUp_->setText( QStringLiteral( "+" ) );
    octaveUp_->setToolTip( tr( "Octave up" ) );
    row->addWidget( octaveUp_ );
    connect( octaveUp_, &QToolButton::clicked,
             this, [this] { setOctave( octave_ + 1 ); } );

    row->addWidget( new QLabel( tr( "Vel:" ), this ) );
    velocitySpin_ = new QSpinBox( this );
    velocitySpin_->setRange( 1, 127 );
    velocitySpin_->setValue( 100 );
    row->addWidget( velocitySpin_ );
    row->addStretch( 1 );

    QVBoxLayout *outer = new QVBoxLayout( this );
    outer->setContentsMargins( 0, 0, 0, 0 );
    outer->setSpacing( 0 );
    outer->addLayout( row );
    outer->addStretch( 1 );

    setOctave( octave_ );
}

void SVirtualKeyboardDock::setOctave( int octave )
{
    if( octave < -1 ) octave = -1;
    if( octave > 8 )  octave = 8;
    octave_ = octave;
    if( octaveLabel_ )
        octaveLabel_->setText( QStringLiteral( "C%1" ).arg( octave_ ) );
    update();
}

double SVirtualKeyboardDock::velocity() const
{
    return velocitySpin_ ? (double) velocitySpin_->value() : 100.0;
}

int SVirtualKeyboardDock::noteForKey( int qtKey ) const
{
    for( const KeyMap &m : kMap )
        if( m.qtKey == qtKey ) {
            const int n = baseKey() + m.semitone;
            return ( n >= 0 && n <= 127 ) ? n : -1;
        }
    return -1;
}

QString SVirtualKeyboardDock::describe() const
{
    return QStringLiteral( "octave=%1|velocity=%2|keys=%3|last=%4" )
        .arg( octave_ ).arg( (int) velocity() )
        .arg( OCTAVES * 12 ).arg( lastKey_ );
}

// ---------------------------------------------------------------------------
// The one write path
// ---------------------------------------------------------------------------

bool SVirtualKeyboardDock::pressNote( int midiKey, double velocity,
                                      qint64 durationTicks )
{
    if( midiKey < 0 || midiKey > 127 ) return false;

    SProject *project = SApplication::app().getCurrentProject();
    if( !project ) return false;
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return false;

    const QList<int> path = targetEventClip();
    if( path.isEmpty() ) return false;

    SLink *link = splacements::placementAt( mixer, path );
    if( !link ) return false;
    SObject *obj = &link->getSObject();
    if( SClipWindow *w = obj->windowTakeAt( -1 ) ) obj = &w->asObject();
    SMidiCut *cut = dynamic_cast<SMidiCut *>( obj );
    if( !cut ) return false;

    // The locator, expressed in the CLIP's content ticks. One conversion,
    // inside the cut, exactly as POSITION_DOMAINS rule 7 requires.
    const offset_t locator = SApplication::app().getGlobalLocatorPos();
    const offset_t start = link->hasStartTime() ? link->getStartTime() : 0;
    const qint64 rel = (qint64) locator - (qint64) start;
    const Fraction relFrames( rel > 0 ? (int64_t) rel : (int64_t) 0 );
    qint64 tick = (qint64) cut->timelineToSourceExact( relFrames ).floorToInt();
    if( tick < 0 ) tick = 0;

    if( durationTicks <= 0 ) durationTicks = 960;
    if( velocity <= 0.0 ) velocity = 100.0;

    SApplication::app().submitAction(
        new SAddNoteAction( path, tick, durationTicks, midiKey, velocity,
                            0, 64.0, -1, true ) );
    lastKey_ = midiKey;
    update();
    return true;
}

// ---------------------------------------------------------------------------
// Painting and input
// ---------------------------------------------------------------------------

void SVirtualKeyboardDock::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    p.fillRect( rect(), QColor( 45, 47, 52 ) );

    const int top = CONTROLS_H;
    const int h = height() - top;
    if( h <= 4 ) return;

    const int nWhite = OCTAVES * 7;
    const double ww = (double) width() / nWhite;

    // White keys first, black keys over them - the only order that draws a
    // keyboard rather than a fence.
    for( int i = 0; i < nWhite; ++i ) {
        const QRect r( (int) ( i * ww ), top, (int) ww + 1, h );
        static const int semis[ 7 ] = { 0, 2, 4, 5, 7, 9, 11 };
        const int note = baseKey() + ( i / 7 ) * 12 + semis[ i % 7 ];
        p.fillRect( r, note == lastKey_ ? QColor( 250, 210, 90 )
                                        : QColor( 235, 235, 235 ) );
        p.setPen( QColor( 60, 60, 60 ) );
        p.drawRect( r.adjusted( 0, 0, -1, -1 ) );
        if( i % 7 == 0 ) {
            p.setPen( QColor( 100, 100, 100 ) );
            p.drawText( r.adjusted( 2, 0, -2, -2 ),
                        Qt::AlignBottom | Qt::AlignHCenter,
                        QStringLiteral( "C%1" ).arg( octave_ + i / 7 ) );
        }
    }
    for( int oct = 0; oct < OCTAVES; ++oct ) {
        for( int s = 0; s < 12; ++s ) {
            if( !isBlack( s ) ) continue;
            const int leftWhite = whiteIndex( s - 1 );
            if( leftWhite < 0 ) continue;
            const double cx = ( oct * 7 + leftWhite + 1 ) * ww;
            const QRect r( (int) ( cx - ww * 0.3 ), top,
                           (int) ( ww * 0.6 ), (int) ( h * 0.62 ) );
            const int note = baseKey() + oct * 12 + s;
            p.fillRect( r, note == lastKey_ ? QColor( 200, 160, 40 )
                                            : QColor( 25, 25, 28 ) );
        }
    }
}

int SVirtualKeyboardDock::noteAtPoint( const QPoint &pt ) const
{
    const int top = CONTROLS_H;
    const int h = height() - top;
    if( pt.y() < top || h <= 0 ) return -1;

    const int nWhite = OCTAVES * 7;
    const double ww = (double) width() / nWhite;
    if( ww <= 0.0 ) return -1;

    // Black keys are on top, so they are hit-tested first.
    if( pt.y() < top + h * 0.62 ) {
        for( int oct = 0; oct < OCTAVES; ++oct ) {
            for( int s = 0; s < 12; ++s ) {
                if( !isBlack( s ) ) continue;
                const int leftWhite = whiteIndex( s - 1 );
                if( leftWhite < 0 ) continue;
                const double cx = ( oct * 7 + leftWhite + 1 ) * ww;
                if( pt.x() >= cx - ww * 0.3 && pt.x() < cx + ww * 0.3 )
                    return baseKey() + oct * 12 + s;
            }
        }
    }
    const int wi = (int) ( pt.x() / ww );
    if( wi < 0 || wi >= nWhite ) return -1;
    static const int semis[ 7 ] = { 0, 2, 4, 5, 7, 9, 11 };
    return baseKey() + ( wi / 7 ) * 12 + semis[ wi % 7 ];
}

void SVirtualKeyboardDock::mousePressEvent( QMouseEvent *ev )
{
    setFocus( Qt::MouseFocusReason );
    const int note = noteAtPoint( ev->position().toPoint() );
    if( note >= 0 ) pressNote( note, velocity(), 960 );
    ev->accept();
}

void SVirtualKeyboardDock::keyPressEvent( QKeyEvent *ev )
{
    // Auto-repeat would insert a note per repeat tick; a held key is one note.
    if( ev->isAutoRepeat() ) { ev->ignore(); return; }

    if( ev->key() == Qt::Key_PageUp )   { setOctave( octave_ + 1 ); ev->accept(); return; }
    if( ev->key() == Qt::Key_PageDown ) { setOctave( octave_ - 1 ); ev->accept(); return; }

    const int note = noteForKey( ev->key() );
    if( note < 0 ) {
        // Space, and everything else this map does not claim, belongs to the
        // shell. ignore() is what lets it get there.
        ev->ignore();
        return;
    }
    pressNote( note, velocity(), 960 );
    ev->accept();
}
