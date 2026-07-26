#include "app/timeline/sclippropertiespanel.h"

#include <limits>

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include "app/shell/sapplication.h"

#include "app/model/sexternfile.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"

#include "app/actions/scompositeaction.h"

#include "app/objects/cut/scut.h"
#include "app/objects/cut/sresizeclipaction.h"
#include "app/objects/cut/ssetclipnameaction.h"
#include "app/objects/cut/ssetformantpreserveaction.h"
#include "app/objects/cut/ssetpitchaction.h"
#include "app/objects/cut/stakestack.h"

#include "tw/core/twfraction.h"

namespace {

// Frame counts are int64 in the model but int in a QSpinBox. INT_MAX frames is
// ~12.4 hours at 48 kHz, which is well past anything the arrangement supports,
// so the clamp is not reachable in practice.
constexpr int kMaxFrames = std::numeric_limits<int>::max();

// The stretch denominator cap SCut::setStretch() uses, so a value typed here
// lands on exactly the same rational a dragged stretch would.
constexpr uint64_t kStretchMaxDen = (uint64_t) 1 << 20;

bool allEqual( const QList<qint64> &v )
{
    for( int i = 1; i < v.size(); ++i ) if( v.at( i ) != v.at( 0 ) ) return false;
    return true;
}

} // namespace

SClipPropertiesPanel::SClipPropertiesPanel( QWidget *parent )
    : QWidget( parent )
{
    // Deliberately NO style sheet: the panel inherits the application palette.
    // A QWidget subclass does not paint a style-sheet background by itself, so
    // declaring one without a PE_Widget paintEvent leaves stale pixels (the
    // diagnosed STrackDetailPanel bug), and an unscoped `QWidget` selector
    // cascades into every child.
    buildUi();
    refresh();
}

SClipPropertiesPanel::~SClipPropertiesPanel() = default;

void SClipPropertiesPanel::buildUi()
{
    QVBoxLayout *outer = new QVBoxLayout( this );
    outer->setContentsMargins( 0, 0, 0, 0 );
    outer->setSpacing( 0 );

    // Shown instead of the form when nothing usable is selected, so the empty
    // panel asks for almost no space (no large fixed sizeHint here).
    placeholder_ = new QLabel( tr( "No clip selected" ), this );
    placeholder_->setAlignment( Qt::AlignCenter );
    placeholder_->setEnabled( false );
    outer->addWidget( placeholder_ );

    scroll_ = new QScrollArea( this );
    scroll_->setWidgetResizable( true );
    scroll_->setFrameShape( QFrame::NoFrame );
    outer->addWidget( scroll_, 1 );

    QWidget *form = new QWidget( scroll_ );
    QVBoxLayout *formLayout = new QVBoxLayout( form );
    formLayout->setContentsMargins( 6, 6, 6, 6 );
    formLayout->setSpacing( 6 );

    // --- Source (read-only) ------------------------------------------------
    QGroupBox *sourceGroup = new QGroupBox( tr( "Source" ), form );
    QFormLayout *sourceForm = new QFormLayout( sourceGroup );

    sourceLabel_ = new QLabel( sourceGroup );
    sourceLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    sourceForm->addRow( tr( "File:" ), sourceLabel_ );

    takesLabel_ = new QLabel( sourceGroup );
    sourceForm->addRow( tr( "Takes:" ), takesLabel_ );

    // The warp-anchor count is read-only, but clearing them is a single
    // undoable window edit — so the button lives with the value it describes.
    QWidget *warpRow = new QWidget( sourceGroup );
    QHBoxLayout *warpLayout = new QHBoxLayout( warpRow );
    warpLayout->setContentsMargins( 0, 0, 0, 0 );
    warpLabel_ = new QLabel( warpRow );
    warpLayout->addWidget( warpLabel_, 1 );
    clearWarpButton_ = new QPushButton( tr( "Clear warp" ), warpRow );
    warpLayout->addWidget( clearWarpButton_ );
    sourceForm->addRow( tr( "Warp:" ), warpRow );

    formLayout->addWidget( sourceGroup );

    // --- Clip --------------------------------------------------------------
    QGroupBox *clipGroup = new QGroupBox( tr( "Clip" ), form );
    QFormLayout *clipForm = new QFormLayout( clipGroup );

    nameEdit_ = new QLineEdit( clipGroup );
    clipForm->addRow( tr( "Name:" ), nameEdit_ );

    startSpin_ = new QSpinBox( clipGroup );
    startSpin_->setRange( 0, kMaxFrames );
    startSpin_->setGroupSeparatorShown( false );
    startSpin_->setToolTip( tr( "Timeline position, in frames at the project rate" ) );
    clipForm->addRow( tr( "Start (frames):" ), startSpin_ );

    durationSpin_ = new QSpinBox( clipGroup );
    // Minimum 1: a duration of 0 means UNBOUNDED to the track mixer, which
    // would bleed source material past the clip end.
    durationSpin_->setRange( 1, kMaxFrames );
    durationSpin_->setGroupSeparatorShown( false );
    clipForm->addRow( tr( "Duration (frames):" ), durationSpin_ );

    formLayout->addWidget( clipGroup );

    // --- Source window -----------------------------------------------------
    QGroupBox *windowGroup = new QGroupBox( tr( "Source window" ), form );
    QFormLayout *windowForm = new QFormLayout( windowGroup );

    slipSpin_ = new QSpinBox( windowGroup );
    slipSpin_->setRange( 0, kMaxFrames );
    slipSpin_->setGroupSeparatorShown( false );
    slipSpin_->setToolTip( tr( "Source anchor: which source frame the clip starts at" ) );
    windowForm->addRow( tr( "Slip (frames):" ), slipSpin_ );

    stretchSpin_ = new QDoubleSpinBox( windowGroup );
    stretchSpin_->setDecimals( 4 );
    stretchSpin_->setRange( 0.1, 10.0 );
    stretchSpin_->setSingleStep( 0.01 );
    stretchSpin_->setGroupSeparatorShown( false );
    stretchSpin_->setToolTip( tr( "Output/input duration ratio (>1 = longer/slower)" ) );
    windowForm->addRow( tr( "Stretch:" ), stretchSpin_ );

    QWidget *pitchRow = new QWidget( windowGroup );
    QHBoxLayout *pitchLayout = new QHBoxLayout( pitchRow );
    pitchLayout->setContentsMargins( 0, 0, 0, 0 );
    pitchSpin_ = new QSpinBox( pitchRow );
    pitchSpin_->setRange( -(int) SCut::PITCH_CENTS_LIMIT,
                           (int) SCut::PITCH_CENTS_LIMIT );
    pitchSpin_->setSingleStep( 100 );
    pitchSpin_->setGroupSeparatorShown( false );
    pitchLayout->addWidget( pitchSpin_, 1 );
    pitchStLabel_ = new QLabel( pitchRow );
    pitchStLabel_->setMinimumWidth( 70 );
    pitchLayout->addWidget( pitchStLabel_ );
    windowForm->addRow( tr( "Pitch (cents):" ), pitchRow );

    formLayout->addWidget( windowGroup );

    // --- Playback ----------------------------------------------------------
    QGroupBox *playGroup = new QGroupBox( tr( "Playback" ), form );
    QFormLayout *playForm = new QFormLayout( playGroup );

    QWidget *loopRow = new QWidget( playGroup );
    QHBoxLayout *loopLayout = new QHBoxLayout( loopRow );
    loopLayout->setContentsMargins( 0, 0, 0, 0 );
    loopSpin_ = new QSpinBox( loopRow );
    loopSpin_->setRange( 0, kMaxFrames );
    loopSpin_->setGroupSeparatorShown( false );
    loopSpin_->setToolTip( tr( "Length of the repeating segment; 0 = no loop" ) );
    loopLayout->addWidget( loopSpin_, 1 );
    clearLoopButton_ = new QPushButton( tr( "Clear loop" ), loopRow );
    loopLayout->addWidget( clearLoopButton_ );
    playForm->addRow( tr( "Loop (frames):" ), loopRow );

    formantCheck_ = new QCheckBox( tr( "Preserve formants" ), playGroup );
    formantCheck_->setTristate( true );
    formantCheck_->setToolTip(
        tr( "Vocoder formant preservation; audible only on transposed clips" ) );
    playForm->addRow( QString(), formantCheck_ );

    formLayout->addWidget( playGroup );
    formLayout->addStretch( 1 );

    scroll_->setWidget( form );

    // Commits happen ONLY on editingFinished / clicked. valueChanged and
    // textChanged fire while the user is still typing (and, for a programmatic
    // write, would turn a refresh into an edit) — they are used here purely to
    // record that the user touched the field.
    connect( nameEdit_, &QLineEdit::textEdited,
             this, [this]{ markEdited( nameEdit_ ); } );
    connect( nameEdit_, &QLineEdit::editingFinished,
             this, &SClipPropertiesPanel::commitName );

    const auto wireSpin = [this]( QSpinBox *sp, void (SClipPropertiesPanel::*slot)() ) {
        connect( sp, &QSpinBox::textChanged, this, [this, sp]{ markEdited( sp ); } );
        connect( sp, &QSpinBox::editingFinished, this, slot );
    };
    wireSpin( startSpin_,    &SClipPropertiesPanel::commitStartTime );
    wireSpin( durationSpin_, &SClipPropertiesPanel::commitDuration );
    wireSpin( slipSpin_,     &SClipPropertiesPanel::commitSlip );
    wireSpin( pitchSpin_,    &SClipPropertiesPanel::commitPitch );
    wireSpin( loopSpin_,     &SClipPropertiesPanel::commitLoopLength );

    connect( stretchSpin_, &QDoubleSpinBox::textChanged,
             this, [this]{ markEdited( stretchSpin_ ); } );
    connect( stretchSpin_, &QDoubleSpinBox::editingFinished,
             this, &SClipPropertiesPanel::commitStretch );

    // Live semitone readout: display only, never a commit.
    connect( pitchSpin_, QOverload<int>::of( &QSpinBox::valueChanged ),
             this, [this]( int cents ) {
                 pitchStLabel_->setText(
                     QString::asprintf( "%+.2f st", cents / 100.0 ) );
             } );

    connect( clearLoopButton_, &QPushButton::clicked,
             this, &SClipPropertiesPanel::onClearLoop );
    connect( clearWarpButton_, &QPushButton::clicked,
             this, &SClipPropertiesPanel::onClearWarp );
    // clicked(), NOT stateChanged(): clicked fires only on real user
    // interaction, so a refresh can never re-trigger a commit.
    connect( formantCheck_, &QCheckBox::clicked,
             this, &SClipPropertiesPanel::onFormantClicked );
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

QList<SClipPropertiesPanel::ClipRef> SClipPropertiesPanel::collectClips() const
{
    QList<ClipRef> out;

    SApplication &app = SApplication::app();
    SProject *project = app.getCurrentProject();
    if( !project ) return out;
    SObject *mixer = splacements::rootContainer( project );
    if( !mixer ) return out;

    // Paths, not the cached SLink list, are the authority: a path is re-resolved
    // against the live tree on every call, so a link destroyed since the last
    // refresh simply drops out instead of dangling.
    const QList<QList<int>> paths = app.getCurrentSelectionPaths();
    for( const QList<int> &p : paths ) {
        SLink *link = splacements::placementAt( mixer, p );
        if( !link ) continue;

        ClipRef ref;
        ref.path = p;
        ref.link = link;
        ref.cut  = dynamic_cast<SCut*>( &link->getSObject() );
        if( !ref.cut ) {
            // A take stack presents its ACTIVE take (the resolution every
            // per-clip action uses when take == -1).
            if( STakeStack *stack =
                    dynamic_cast<STakeStack*>( &link->getSObject() ) ) {
                ref.cut       = stack->activeCut();
                ref.takeIndex = stack->activeTakeIndex();
                ref.nTakes    = stack->nTakes();
            }
        }
        if( !ref.cut ) continue;   // neither a cut nor a stack: not our business
        out.append( ref );
    }
    return out;
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

bool SClipPropertiesPanel::consumeEdited( QObject *w )
{
    return edited_.remove( w );
}

// Blanking uses QAbstractSpinBox::clear() rather than the lineEdit(): that
// accessor is PROTECTED. clear() empties the input field and sets the widget's
// "cleared" flag; the next setValue() resets that flag and re-renders the text
// unconditionally (QAbstractSpinBoxPrivate::setValue always calls updateEdit),
// so a mixed field correctly repopulates even when the shared value happens to
// equal the internal one.
void SClipPropertiesPanel::showValues( QSpinBox *sp, const QList<qint64> &values )
{
    // Never overwrite what the user is in the middle of typing.
    if( sp->hasFocus() && edited_.contains( sp ) ) return;

    QSignalBlocker block( sp );
    if( values.isEmpty() || !allEqual( values ) ) {
        sp->clear();
        return;
    }
    sp->setValue( (int) qBound<qint64>( (qint64) sp->minimum(), values.first(),
                                        (qint64) sp->maximum() ) );
}

void SClipPropertiesPanel::showValues( QDoubleSpinBox *sp, const QList<double> &values )
{
    if( sp->hasFocus() && edited_.contains( sp ) ) return;

    QSignalBlocker block( sp );
    bool same = !values.isEmpty();
    for( int i = 1; i < values.size(); ++i )
        if( values.at( i ) != values.at( 0 ) ) { same = false; break; }
    if( !same ) {
        sp->clear();
        return;
    }
    sp->setValue( qBound( sp->minimum(), values.first(), sp->maximum() ) );
}

void SClipPropertiesPanel::showValues( QLineEdit *le, const QStringList &values )
{
    if( le->hasFocus() && edited_.contains( le ) ) return;

    QSignalBlocker block( le );
    if( values.isEmpty() ) { le->clear(); return; }
    bool same = true;
    for( int i = 1; i < values.size(); ++i )
        if( values.at( i ) != values.at( 0 ) ) { same = false; break; }
    le->setText( same ? values.first() : QString() );
}

void SClipPropertiesPanel::showValues( QCheckBox *cb, const QList<bool> &values )
{
    QSignalBlocker block( cb );
    if( values.isEmpty() ) {
        cb->setTristate( false );
        cb->setCheckState( Qt::Unchecked );
        return;
    }
    bool same = true;
    for( int i = 1; i < values.size(); ++i )
        if( values.at( i ) != values.at( 0 ) ) { same = false; break; }
    cb->setTristate( !same );
    cb->setCheckState( !same ? Qt::PartiallyChecked
                             : ( values.first() ? Qt::Checked : Qt::Unchecked ) );
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void SClipPropertiesPanel::refresh()
{
    // arrangementChanged fires for EVERY committed action, including the one
    // this panel just submitted; the guard keeps that from re-entering through
    // a widget signal we did not manage to block.
    if( updating_ ) return;
    updating_ = true;

    const QList<ClipRef> clips = collectClips();

    if( clips.isEmpty() ) {
        placeholder_->setVisible( true );
        scroll_->setVisible( false );
        edited_.clear();
        updating_ = false;
        return;
    }
    placeholder_->setVisible( false );
    scroll_->setVisible( true );

    const bool single = ( clips.size() == 1 );

    // --- Source (read-only) ---
    QStringList sources;
    bool anyWarp = false;
    qint64 warpCount = -1;
    bool warpSame = true;
    QStringList takeTexts;
    for( const ClipRef &c : clips ) {
        if( SExternFile *xf = dynamic_cast<SExternFile*>( &c.cut->getContent() ) ) {
            const QString fn = xf->getFileName();
            sources << ( fn.isEmpty() ? tr( "(unnamed file)" )
                                      : QFileInfo( fn ).fileName() );
        } else {
            // A container-backed clip (group/mixer render, registered asset)
            // has no file behind it.
            sources << tr( "(asset)" );
        }

        const qint64 n = (qint64) c.cut->getGrainParams().warpAnchors.size();
        if( n > 0 ) anyWarp = true;
        if( warpCount < 0 ) warpCount = n;
        else if( warpCount != n ) warpSame = false;

        if( c.nTakes <= 0 )                 // a plain clip, not a stack
            takeTexts << tr( "single" );
        else if( c.takeIndex < 0 )          // a stack with nothing audible
            takeTexts << tr( "no active take (of %1)" ).arg( c.nTakes );
        else
            takeTexts << tr( "take %1 of %2" ).arg( c.takeIndex + 1 )
                                              .arg( c.nTakes );
    }

    bool sourcesSame = true;
    for( int i = 1; i < sources.size(); ++i )
        if( sources.at( i ) != sources.at( 0 ) ) { sourcesSame = false; break; }
    sourceLabel_->setText( sourcesSame ? sources.first()
                                       : tr( "(multiple sources)" ) );

    bool takesSame = true;
    for( int i = 1; i < takeTexts.size(); ++i )
        if( takeTexts.at( i ) != takeTexts.at( 0 ) ) { takesSame = false; break; }
    takesLabel_->setText( takesSame ? takeTexts.first() : tr( "(mixed)" ) );

    warpLabel_->setText( warpSame ? tr( "%1 anchor(s)" ).arg( warpCount )
                                  : tr( "(mixed)" ) );
    clearWarpButton_->setEnabled( anyWarp );

    // --- Editable fields ---
    QStringList names;
    QList<qint64> starts, durations, slips, pitches, loops;
    QList<double> stretches;
    QList<bool>   formants;
    bool anyLoop = false;
    for( const ClipRef &c : clips ) {
        names      << c.cut->getSName();
        starts     << (qint64) c.link->getStartTime();
        durations  << (qint64) c.cut->getDurationBlocking();
        slips      << c.cut->getSrcStart().floorToInt();
        stretches  << c.cut->getStretch();
        pitches    << (qint64) c.cut->getPitchCents();
        const qint64 loop = c.cut->getLoopLength().frames();
        loops      << loop;
        if( loop > 0 ) anyLoop = true;
        formants   << c.cut->getPreserveFormants();
    }

    // Name and start time are IDENTITY, not shared quantities: setting them
    // across a multi-selection would stack every clip at one spot / give them
    // all one name. Both are single-selection edits.
    nameEdit_->setEnabled( single );
    startSpin_->setEnabled( single );

    showValues( nameEdit_,     names );
    showValues( startSpin_,    starts );
    showValues( durationSpin_, durations );
    showValues( slipSpin_,     slips );
    showValues( stretchSpin_,  stretches );
    showValues( pitchSpin_,    pitches );
    showValues( loopSpin_,     loops );
    showValues( formantCheck_, formants );

    // The semitone readout mirrors the pitch field, including its blank state.
    bool pitchSame = allEqual( pitches );
    pitchStLabel_->setText( pitchSame
        ? QString::asprintf( "%+.2f st", pitches.first() / 100.0 )
        : QStringLiteral( "--" ) );

    clearLoopButton_->setEnabled( anyLoop );

    updating_ = false;
}

void SClipPropertiesPanel::focusFirstField()
{
    // First ENABLED editable field, in visual order.
    QList<QWidget*> order{ nameEdit_, startSpin_, durationSpin_, slipSpin_,
                           stretchSpin_, pitchSpin_, loopSpin_, formantCheck_ };
    // isVisibleTo(): the panel itself may not be mapped yet when the dock hands
    // us focus, but the form's own show/hide state is already correct.
    if( !scroll_->isVisibleTo( this ) ) return;
    for( QWidget *w : order ) {
        if( !w || !w->isEnabled() ) continue;
        w->setFocus( Qt::OtherFocusReason );
        if( QLineEdit *le = qobject_cast<QLineEdit*>( w ) ) le->selectAll();
        else if( QAbstractSpinBox *sp = qobject_cast<QAbstractSpinBox*>( w ) )
            sp->selectAll();
        return;
    }
}

// ---------------------------------------------------------------------------
// Commits
//
// Every one of these follows the same shape as the timeline gestures: resolve
// the selection FRESH, build one action per clip that actually changes, and
// submit them as ONE composite so N clips are one undo step. A no-op action is
// never submitted — a composite whose apply() is rejected fails the whole
// action, which breaks the headless test cases.
// ---------------------------------------------------------------------------

void SClipPropertiesPanel::submitComposite( SCompositeAction *composite )
{
    if( composite->count() > 0 ) SApplication::app().submitAction( composite );
    else delete composite;
}

void SClipPropertiesPanel::commitName()
{
    if( updating_ ) return;
    if( !consumeEdited( nameEdit_ ) ) return;

    const QList<ClipRef> clips = collectClips();
    if( clips.size() != 1 ) return;   // identity edit: single selection only

    const QString name = nameEdit_->text();
    if( name == clips.first().cut->getSName() ) return;
    SApplication::app().submitAction(
        new SSetClipNameAction( clips.first().path, name ) );
}

void SClipPropertiesPanel::commitStartTime()
{
    if( updating_ ) return;
    if( !consumeEdited( startSpin_ ) ) return;
    if( startSpin_->text().trimmed().isEmpty() ) return;

    const QList<ClipRef> clips = collectClips();
    if( clips.size() != 1 ) return;   // identity edit: single selection only

    const ClipRef &c = clips.first();
    const qint64 start = startSpin_->value();
    if( start == (qint64) c.link->getStartTime() ) return;

    // The whole window goes through in one action; every value except the one
    // being edited is read off the clip and passed through unchanged.
    SApplication::app().submitAction(
        new SResizeClipAction( c.path, (offset_t) start, c.cut->getSrcStart(),
                               c.cut->getDurationBlocking(),
                               (length_t) c.cut->getLoopLength().frames(),
                               c.cut->getStretchExact() ) );
}

void SClipPropertiesPanel::commitDuration()
{
    if( updating_ ) return;
    if( !consumeEdited( durationSpin_ ) ) return;
    if( durationSpin_->text().trimmed().isEmpty() ) return;

    const qint64 duration = durationSpin_->value();
    if( duration <= 0 ) return;

    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        if( duration == (qint64) c.cut->getDurationBlocking() ) continue;
        composite->append(
            new SResizeClipAction( c.path, c.link->getStartTime(),
                                   c.cut->getSrcStart(), (length_t) duration,
                                   (length_t) c.cut->getLoopLength().frames(),
                                   c.cut->getStretchExact() ) );
    }
    submitComposite( composite );
}

void SClipPropertiesPanel::commitSlip()
{
    if( updating_ ) return;
    if( !consumeEdited( slipSpin_ ) ) return;
    if( slipSpin_->text().trimmed().isEmpty() ) return;

    const qint64 slip = slipSpin_->value();

    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        // The display is the FLOORED anchor, so compare floored: a legacy
        // fractional anchor must not be rewritten just because it was shown
        // rounded.
        if( slip == c.cut->getSrcStart().floorToInt() ) continue;
        composite->append(
            new SResizeClipAction( c.path, c.link->getStartTime(),
                                   Fraction( slip ), c.cut->getDurationBlocking(),
                                   (length_t) c.cut->getLoopLength().frames(),
                                   c.cut->getStretchExact() ) );
    }
    submitComposite( composite );
}

void SClipPropertiesPanel::commitStretch()
{
    if( updating_ ) return;
    if( !consumeEdited( stretchSpin_ ) ) return;
    if( stretchSpin_->text().trimmed().isEmpty() ) return;

    // Same rounding as SCut::setStretch(), so a typed 0.5 is the same rational
    // a dragged stretch would produce.
    const Fraction stretch =
        doubleToFractionWithLookup( stretchSpin_->value() ).limitedTo( kStretchMaxDen );
    if( stretch <= Fraction( 0 ) ) return;

    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        if( stretch == c.cut->getStretchExact() ) continue;
        composite->append(
            new SResizeClipAction( c.path, c.link->getStartTime(),
                                   c.cut->getSrcStart(), c.cut->getDurationBlocking(),
                                   (length_t) c.cut->getLoopLength().frames(),
                                   stretch ) );
    }
    submitComposite( composite );
}

void SClipPropertiesPanel::commitPitch()
{
    if( updating_ ) return;
    if( !consumeEdited( pitchSpin_ ) ) return;
    if( pitchSpin_->text().trimmed().isEmpty() ) return;

    const double cents = SCut::clampPitchCents( (double) pitchSpin_->value() );

    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        // Already at the clamped value: submitting would push an undo step
        // that changes nothing.
        if( cents == c.cut->getPitchCents() ) continue;
        composite->append( new SSetPitchAction( c.path, cents ) );
    }
    submitComposite( composite );
}

void SClipPropertiesPanel::commitLoopLength()
{
    if( updating_ ) return;
    if( !consumeEdited( loopSpin_ ) ) return;
    if( loopSpin_->text().trimmed().isEmpty() ) return;

    const qint64 loop = loopSpin_->value();

    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        if( loop == c.cut->getLoopLength().frames() ) continue;
        composite->append(
            new SResizeClipAction( c.path, c.link->getStartTime(),
                                   c.cut->getSrcStart(), c.cut->getDurationBlocking(),
                                   (length_t) loop, c.cut->getStretchExact() ) );
    }
    submitComposite( composite );
}

void SClipPropertiesPanel::onClearLoop()
{
    if( updating_ ) return;

    // The clip KEEPS its duration — it plays its content once and falls silent
    // for the remainder — so nothing else on the timeline shifts.
    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        if( c.cut->getLoopLength().frames() == 0 ) continue;
        composite->append(
            new SResizeClipAction( c.path, c.link->getStartTime(),
                                   c.cut->getSrcStart(), c.cut->getDurationBlocking(),
                                   0, c.cut->getStretchExact() ) );
    }
    submitComposite( composite );
}

void SClipPropertiesPanel::onClearWarp()
{
    if( updating_ ) return;

    // Anchors ride on the same window action (its opt-in warp payload), so
    // dropping them is one undo step with the window it belongs to.
    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        if( c.cut->getGrainParams().warpAnchors.empty() ) continue;
        SResizeClipAction *a =
            new SResizeClipAction( c.path, c.link->getStartTime(),
                                   c.cut->getSrcStart(), c.cut->getDurationBlocking(),
                                   (length_t) c.cut->getLoopLength().frames(),
                                   c.cut->getStretchExact() );
        a->setWarpAnchors( {} );   // empty = clear
        composite->append( a );
    }
    submitComposite( composite );
}

void SClipPropertiesPanel::onFormantClicked( bool )
{
    if( updating_ ) return;

    // Leaving the mixed state: the tri-state CYCLE would let the user land back
    // on PartiallyChecked, which is not a value we can commit. A click on a
    // mixed box therefore means "turn it on for all", and the box goes binary.
    if( formantCheck_->checkState() == Qt::PartiallyChecked ) {
        QSignalBlocker block( formantCheck_ );
        formantCheck_->setCheckState( Qt::Checked );
    }
    formantCheck_->setTristate( false );
    const bool on = formantCheck_->isChecked();

    SCompositeAction *composite = new SCompositeAction;
    for( const ClipRef &c : collectClips() ) {
        if( c.cut->getPreserveFormants() == on ) continue;
        composite->append( new SSetFormantPreserveAction( c.path, on ) );
    }
    submitComposite( composite );
}
