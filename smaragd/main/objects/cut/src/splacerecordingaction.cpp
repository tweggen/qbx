#include "app/objects/cut/splacerecordingaction.h"
#include "app/objects/cut/saudiopassverbs.h"
#include "app/objects/recplan/srecordplanner.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/model/slink.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

using namespace strackpath;

SPlaceRecordingAction::SPlaceRecordingAction( const QList<int> &trackPath,
                                              const QString &filePath,
                                              offset_t timePos,
                                              offset_t srcOffset,
                                              length_t length,
                                              const QString &mode,
                                              length_t extent, bool armed,
                                              offset_t groupStart,
                                              length_t groupLength,
                                              const QString &takeStamp )
    : mode_( SProjectProps::isRecordMode( mode ) ? mode : QString() ),
      armed_( armed ),
      trackPath_( trackPath ), filePath_( filePath ), timePos_( timePos ),
      srcOffset_( srcOffset ), length_( length ), extent_( extent ),
      groupStart_( groupStart ), groupLength_( groupLength ),
      takeStamp_( takeStamp )
{
}

SPlaceRecordingAction::~SPlaceRecordingAction()
{
    delete plan_;      // and with it every child action, and any pin one holds
}

SApplyResult SPlaceRecordingAction::apply( SProject *project )
{
    if( !project || filePath_.isEmpty() ) {
        return {false, nullptr};
    }
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SObject *lane = splacements::placementLaneAt( mixer, trackPath_, "place-recording" );
    if( !lane ) {
        return {false, nullptr};
    }
    QString mutablePath = filePath_;
    SLink *wavLink = project->linkToFile( mutablePath );
    if( !wavLink ) {
        return {false, nullptr};
    }
    // The link is a planning temp only (the planned place-clip/add-take
    // sub-actions create their own links); read the duration and release it —
    // an unparented leftover link would pin the wave forever.
    const bool hasDur = wavLink->getSObject().hasDuration();
    // Blocking reads throughout this planner (P19): the whole gap/take plan is
    // derived from these durations; a stale try-lock value mis-plans it.
    const length_t waveDur =
        hasDur ? wavLink->getSObject().getDurationBlocking() : 0;
    delete wavLink;
    if( !hasDur || waveDur == 0 ) {
        return {false, nullptr};
    }
    // The SUB-RANGE of the recording this call places (proposal 21 L3b): the
    // whole file by default, one loop pass when the recorder is cycling.
    // A NEGATIVE SOURCE OFFSET IS LEGAL AND MEANS LEADING SILENCE, which is
    // what lets a placement carry material that starts PARTWAY IN.
    //
    // It used to be clamped to 0 here, and that clamp is what made loop
    // recording unable to express a pass that STARTED LATE. A pass beginning
    // mid-cycle needs its material pushed later inside a take that spans the
    // whole cycle region -- i.e. exactly a negative anchor, which `SCut` has
    // supported since proposal 23 ("the clip then opens with silence and its
    // data starts later"). The machinery already handled a pass that ENDED
    // early, because running past the end of a sample is silence too; the
    // asymmetry was this one line.
    const offset_t srcOff = srcOffset_;
    if( srcOff >= (offset_t) waveDur ) return {false, nullptr};
    length_t span = (length_t)( (offset_t) waveDur - srcOff );
    if( length_ > 0 && length_ < span ) span = length_;

    // FROM HERE THIS VERB DECIDES NOTHING (plan 53 §5.1, QBX-171). It reads the
    // lane ONCE, picks the planner for the mode it was HANDED -- it cannot
    // resolve one, see srecordmode.h -- and applies what the planner appended.
    srecplan::Span sp;
    sp.filePath  = filePath_;
    sp.a         = timePos_;
    sp.b         = timePos_ + (offset_t) span;
    sp.srcOffset = srcOff;
    // THE TAKE'S EXTENT (plan 53 §3.1 step 5, QBX-180), which the recorder
    // knows and the span cannot: `span` above is clamped to the MATERIAL, so a
    // cycle pass that stopped early has `b` short of the cycle end while its
    // take must still cover the whole cycle. QBX-171 left a `fromCycle`
    // boolean here for this and a boolean cannot carry it -- see the header.
    //
    // Only mode A reads it. Mode B keeps X8 ("no take beyond the cycle") for
    // free: the pass span IS the cycle and `TakesSplit` never places or stacks
    // anything outside `[a, b)` -- it cuts AT `b` precisely so that what lies
    // beyond stays its own child (`rec_split_cycle_inside` asserts that, two
    // passes in).
    sp.extent = extent_;

    srecplan::Context ctx;
    ctx.mode  = srecplan::modeFromString( mode_ );
    ctx.armed = armed_;
    // THE GROUP EXTENT, absent unless the recorder named one (plan 53 D4 (a),
    // QBX-182). Absent is every call written before P4, and every planner then
    // behaves exactly as it did.
    if( groupLength_ > 0 ) {
        ctx.group.a = groupStart_;
        ctx.group.b = groupStart_ + (offset_t) groupLength_;
    }
    ctx.takeStamp = takeStamp_;
    // This verb places AUDIO. `place-midi-recording` is the event sibling and
    // passes Event; the planners branch on nothing else.
    ctx.kind  = SContentKind::Audio;
    // HOW the planner's decisions are spelled: in this module's verbs
    // (QBX-188). The planner itself may not name them — it serves the event
    // side too, which may not depend on `objects/cut`.
    const SAudioPassVerbs audioVerbs;
    ctx.verbs = &audioVerbs;

    const srecplan::LaneSnapshot snap = srecplan::snapshot( lane, trackPath_ );

    // HELD, NOT STACK-LOCAL -- see the note on `plan_`. A child's inverse may
    // BORROW the child (`remove-sample` pins a take column and its
    // `restore-pinned-clip` reads the pin back through the child), so the
    // children have to outlive the inverse this returns, and
    // `SActionUndoCommand` keeps this action for exactly that long.
    delete plan_;                       // a redo re-plans from scratch
    plan_ = new SCompositeAction();
    SCompositeAction &composite = *plan_;
    QString why;
    if( !srecplan::plannerFor( ctx.mode ).plan( snap, sp, ctx, composite,
                                                &why ) ) {
        // REFUSED BEFORE APPLY: nothing was appended, so there is nothing to
        // roll back. Announced, because a refusal that is merely a false
        // return is indistinguishable from the pass having landed (plan 53 B6
        // routes this through the UI as well; that is QBX-164's).
        qWarning( "place-recording: lane refused the pass in mode '%s': %s",
                  srecplan::modeToString( ctx.mode ).toStdString().c_str(),
                  why.toStdString().c_str() );
        return {false, nullptr};
    }
    // AN EMPTY PLAN APPLIES AS SUCCESS, which is right here and was a bug in
    // the old planner: there, emptiness meant the take had been thrown away.
    // The planners refuse an empty span rather than returning an empty plan,
    // so the only way to get here with nothing is `armed="0"` -- either with
    // no group extent (the pre-P4 "nothing to do"), or with one over a member
    // that holds no material of this kind inside `C_g` and so has no column to
    // keep in step (plan 53 §3.4, QBX-182).
    return composite.apply( project );
}

void SPlaceRecordingAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "timePos", QString::fromStdString(
                           Fraction( timePos_, 1 ).toString() ) );
    // Only when non-default: the round-trip audit requires that an attribute a
    // fixture does not declare must not appear.
    if( srcOffset_ != 0 )
        elem.setAttribute( "srcOffset", QString::fromStdString(
                               Fraction( srcOffset_, 1 ).toString() ) );
    if( length_ != 0 )
        elem.setAttribute( "length", QString::fromStdString(
                               Fraction( length_, 1 ).toString() ) );
    if( extent_ != 0 )
        elem.setAttribute( "extent", QString::fromStdString(
                               Fraction( extent_, 1 ).toString() ) );
    // Same non-default-only rule: an attribute a fixture did not declare must
    // not appear, or the round-trip audit fails.
    if( !mode_.isEmpty() )
        elem.setAttribute( "mode", mode_ );
    // Same rule: armed="1" is the default, so only the un-armed case writes.
    if( !armed_ )
        elem.setAttribute( "armed", "0" );
    // The group extent and the stamp, non-default only (same rule).
    if( groupLength_ > 0 ) {
        elem.setAttribute( "groupStart", QString::fromStdString(
                               Fraction( groupStart_, 1 ).toString() ) );
        elem.setAttribute( "groupLength", QString::fromStdString(
                               Fraction( groupLength_, 1 ).toString() ) );
    }
    if( !takeStamp_.isEmpty() )
        elem.setAttribute( "takeStamp", takeStamp_ );
}

bool SPlaceRecordingAction::readXml( const QDomElement &elem, int /*version*/ )
{
    trackPath_ = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    filePath_ = elem.attribute( "filePath", "" );
    // An UNKNOWN mode reads as EMPTY rather than being carried: empty means
    // "whatever this build would do anyway", which is the safe answer for a
    // value no planner can name. Validated here so a typo in a .qxa cannot
    // reach a planner in P1.
    {
        const QString m = elem.attribute( "mode", "" );
        mode_ = SProjectProps::isRecordMode( m ) ? m : QString();
    }
    // An UN-ARMED edit-group member (plan 53 Q5). In P1 every planner treats
    // it as nothing to do; P4 is where the takes planners grow the
    // structure-only plan. Anything but "0" is armed, which keeps every
    // existing call and every existing script armed.
    armed_ = elem.attribute( "armed", "1" ) != QLatin1String( "0" );
    timePos_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    srcOffset_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "srcOffset", "0" ).toStdString() ).toDouble();
    length_ = (length_t)parseFractionOrDouble(
        elem.attribute( "length", "0" ).toStdString() ).toDouble();
    // The TAKE's extent, not the material's (see the header). 0 = the placed
    // span, which is what every call written before QBX-180 means.
    extent_ = (length_t)parseFractionOrDouble(
        elem.attribute( "extent", "0" ).toStdString() ).toDouble();
    // THE GROUP EXTENT (plan 53 D4 (a)). `groupLength` 0 or absent = no edit
    // group for this pass, which is every script written before P4.
    groupStart_ = (offset_t)parseFractionOrDouble(
        elem.attribute( "groupStart", "0" ).toStdString() ).toDouble();
    groupLength_ = (length_t)parseFractionOrDouble(
        elem.attribute( "groupLength", "0" ).toStdString() ).toDouble();
    takeStamp_ = elem.attribute( "takeStamp", "" );
    return true;
}

static const bool s_reg_placerecording = (
    SActionRegistry::instance().registerType(
        QStringLiteral("place-recording"),
        []{ return new SPlaceRecordingAction; }
    ), true
);
