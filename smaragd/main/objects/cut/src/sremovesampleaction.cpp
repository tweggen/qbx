#include "app/objects/cut/sremovesampleaction.h"
#include "app/model/splacements.h"
#include "app/objects/cut/saddsampleaction.h"
#include "app/objects/cut/srestorecontainerclipaction.h"
#include "app/model/sobject.h"
#include "app/objects/cut/srestorepinnedclipaction.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/scut.h"      // grain params only (audio-specific)
#include "app/model/slink.h"
#include "app/model/sexternfile.h"
#include "tw/core/twfraction.h"
#include "tw/core/twlog.h"
#include <QDomElement>

SRemoveSampleAction::SRemoveSampleAction(const QList<int> &trackPath, int clipIdx,
                                         const QString &filePath, offset_t timePos)
    : trackPath_(trackPath), clipIndex_(clipIdx), filePath_(filePath), timePos_(timePos)
{
}

SRemoveSampleAction::~SRemoveSampleAction()
{
    // The undo command deletes both halves when the stack discards them; this
    // is where a pin that was never re-applied is finally released, and the
    // content collected. Main-thread only, which addRef/removeRef assert --
    // true here, because a QUndoStack is cleared from the GUI thread.
    dropStalePin();
}

void SRemoveSampleAction::releaseHeld()
{
    if( holdsRef_ && heldObject_ ) {
        heldObject_->removeRef();
    }
    heldObject_ = nullptr;
    holdsRef_   = false;
}

void SRemoveSampleAction::dropStalePin()
{
    releaseHeld();
}

SApplyResult SRemoveSampleAction::apply(SProject *project)
{
    // FIRST, before anything can fail or return: release a pin from a previous
    // apply. See the header -- this action is also add-sample's live inverse.
    dropStalePin();

    if (!project) {
        return {false, nullptr};
    }

    SObject *root = splacements::rootNamed( project, pathRoot_ );
    if (!root || !root->isPathContainer()) {
        return {false, nullptr};
    }

    // Get the lane. Path-addressed, so a track nested inside a folder track
    // resolves as readily as a top-level one.
    SObject *track = splacements::laneAt( root, trackPath_ );
    if (!track) {
        return {false, nullptr};
    }

    // Get the clip at the specified index.
    SLink *clipLink = track->childAt(clipIndex_);
    if (!clipLink) {
        return {false, nullptr};
    }

    // Capture what the inverse needs BEFORE the clip dies. The caller's
    // filePath_ is only a hint (the GUI used to pass an empty string here, so
    // undo asked linkToFile("") for a file that cannot exist: a modal "Unable
    // to load file." and no undo at all). Read it off the clip instead, the
    // same way SRemoveTakeAction does.
    QString filePath = filePath_;
    bool haveWindow = false;
    Fraction srcStart( 0 );
    length_t cutDuration = 0;
    length_t loopLength = 0;
    twGrainParams grain;
    QList<int> containerPath;      // set for a container-backed clip

    if( SClipWindow *win = SClipWindow::of( &clipLink->getSObject() ) ) {
        SObject &content = win->windowContent();
        // Grain params (stretch, pitch, warp anchors, grain/crossfade sizes)
        // are audio-specific — the interface deliberately does not carry them,
        // and the inverse only restores them onto an audio clip.
        SCut *cut = dynamic_cast<SCut *>( &win->asObject() );
        if( SExternFile *xf = dynamic_cast<SExternFile *>( &content ) ) {
            filePath = xf->getFileName();
            // The whole window, so undo restores the clip the user actually had
            // — not a default full-length one. Blocking duration read (P19):
            // the try-lock snapshot can hand back a stale pre-edit value.
            srcStart    = win->contentAnchorExact();
            cutDuration = win->durationBlocking();
            loopLength  = win->loopLength();
            if( cut ) grain = cut->getGrainParams();
            haveWindow  = true;
        } else if( content.isPathContainer() ) {
            // An asset COPY: a cut windowing a track. Nothing in the registry
            // points at it, so undo has to rebuild the cut over the same
            // container. (A placement of the asset BODY itself never reaches
            // this action — the view routes it to SRemoveAssetPlacementAction,
            // whose inverse re-places the body and so keeps asset identity.)
            containerPath = strackpath::pathOf( root, &content );
            srcStart    = win->contentAnchorExact();
            cutDuration = win->durationBlocking();
            loopLength  = win->loopLength();
            if( cut ) grain = cut->getGrainParams();
            haveWindow  = true;
        }
    }

    // DECIDE INVERTIBILITY BEFORE MUTATING (QBX-146). This used to delete the
    // clip first and then discover it could build no inverse, returning
    // {true, nullptr} -- a successful, silent, PERMANENT deletion.
    //
    // It is worse than merely losing undo. SActionHistory::onApplied_ pushes
    // nothing for a null inverse, so the undo stack keeps its earlier entries
    // and the next Ctrl+Z pops an UNRELATED command: undo appears to work
    // while the content stays gone. That is how this reached the author --
    // takes recorded on a track, deleted, "it did not come back".
    //
    // Only two shapes can be rebuilt: file-backed (SExternFile -> re-add the
    // sample) and container-backed (a cut over a track -> rebuild the cut).
    // isPathContainer() is true for exactly SLaneFragment, SStdMixer and
    // STrack -- so a clip whose content is an STakeStack (a take column) or an
    // SRecordingContent is NEITHER, and was silently destroyed.
    const bool canRestoreContainer = !containerPath.isEmpty() && haveWindow;
    const bool canRestoreFile      = !filePath.isEmpty();

    // A TAKE COLUMN is restored by PINNING it rather than by rebuilding it
    // (QBX-146 step a, plan 52). stakes::columnOfLink is the PREDICATE here --
    // it answers "is this clip a column", on both the direct and the wrapped
    // shape -- and is deliberately NOT what gets pinned: it returns the
    // STakeStack either way, so pinning its result would let the WRAPPER die
    // with the link on the wrapped shape, and the wrapper is what carries that
    // clip's automation, volume, fade and pan.
    //
    // Pin the link's own object. The pin is transitive: SCut holds its content
    // through `content_ = new SLink( content, NULL )` (whose ctor addRef()s)
    // and ~SCut deletes it, and the stack holds its takes the same way -- so
    // one addRef() holds wrapper -> stack -> takes.
    //
    // BOTH SHAPES, direct and wrapped, since QBX-155.
    //
    // Step (a) restricted this to the direct shape because re-linking a WRAPPED
    // column crashed at teardown about 1 run in 4, in
    // SApplication::rewireSpeaker -> twComponent::setInput ->
    // oldPlug->getParentLatch().deleteOutput(oldPlug). The cause was NOT in this
    // module and not in the re-link: twLatchOutput held its latch by a
    // non-nullable reference while the design deliberately lets a plug outlive
    // its latch, so a wrapper SCut's component dying took its latch with it and
    // left the track's consumer holding a plug into freed memory. ~twLatch now
    // orphans its outputs and every consumer can see it (QBX-155).
    //
    // The asymmetry is explained by the same thing: on the wrapped shape the
    // placement's object is the wrapper SCut, which owns its OWN component and
    // therefore its own latch, so deleting the clip tears that latch down; on
    // the direct shape the object is the STakeStack, whose content component is
    // shared and survives. That is also why removing the disconnect above made
    // the wrapped shape WORSE rather than better -- it was never the cause.
    //
    // So columnOfLink is both the predicate AND the reach now. What still
    // refuses below is a live SRecordingContent, for ownership.
    const bool canPinColumn = stakes::columnOfLink( clipLink ) != nullptr;

    // THE PIN IS THE FALLBACK FOR EVERY OTHER CONTENT (QBX-202), and what is
    // left refused is a LIVE RECORDING -- nothing else.
    //
    // Step (a) reached only a take column, so every other un-rebuildable shape
    // kept the refusal, and the commonest of those is an ordinary MIDI clip:
    // its content is an SMidiSequence, which is not an SExternFile, not a path
    // container and not a column. Deleting a recorded MIDI clip therefore did
    // nothing at all but write this warning -- the author's report, QBX-202.
    //
    // There was never a fidelity reason for the narrow reach. The pin does not
    // rebuild anything and so needs to know nothing about what it holds: it
    // addRef()s the object, and the inverse re-links THAT object. The
    // conservative thing is the ORDER, not the reach -- the two rebuild
    // branches keep their priority, so every clip that could be deleted before
    // this change is restored exactly as it was before it, and only the shapes
    // that were refused take the new path.
    //
    // isLiveRecording() is the predicate, which SCut forwards to its content,
    // so a growing capture is recognised through its cut without this module
    // naming SRecordingContent's type at all.
    const bool contentIsLiveRecording = clipLink->getSObject().isLiveRecording();
    const bool canPin =
        canPinColumn || ( !canRestoreContainer && !canRestoreFile
                          && !contentIsLiveRecording );

    if( !canRestoreContainer && !canRestoreFile && !canPin ) {
        // REFUSE, and say so. A refusal the user cannot see is the same bug
        // wearing a different face, and this project's rule is that any
        // refusal is announced rather than silent.
        //
        // ONE SHAPE REACHES HERE, and since QBX-202 the reason is ownership
        // ALONE: SAudioRecorder::Armed holds the growing SCut and its SLink as
        // borrowed raw pointers, touches them every tick and deletes them at
        // stop. Removing that clip is a use-after-free IN THE RECORDER, before
        // undo enters the picture -- which is why SExtractArrangementAction
        // and merge-columns refuse a live recording too. No inverse would make
        // it safe, so this is not a placeholder for unfinished work.
        //
        // The MIDI recorder has no such exposure by construction: it buffers
        // its messages and submits `place-midi-recording` once, at the stop,
        // so no growing MIDI clip is ever in the model to delete.
        const SObject *content = nullptr;
        if( SClipWindow *w = SClipWindow::of( &clipLink->getSObject() ) ) {
            content = &w->windowContent();
        }
        TW_LOGW( "cut",
                 "remove-sample: refusing to delete clip %d on '%s' -- it is a "
                 "LIVE RECORDING (%s). A growing clip belongs to the recorder "
                 "until stop; every other content is restored by identity. "
                 "QBX-146, QBX-202.",
                 clipIndex_,
                 strackpath::qualifiedToString( pathRoot_, trackPath_ )
                     .toUtf8().constData(),
                 content ? content->metaObject()->className() : "unknown" );
        return {false, nullptr};
    }

    // PIN BEFORE DELETING, so the refcount never reaches zero: then
    // deleteLater(), deletePending_ and SObject::event()'s resurrection path
    // never enter the picture at all.
    if( canPin ) {
        SObject *obj = &clipLink->getSObject();
        obj->addRef();
        heldObject_ = obj;
        holdsRef_   = true;

        // WHAT THE LINK ITSELF CARRIED (QBX-202). The pin holds the CONTENT;
        // this SLink is about to be destroyed and the inverse builds a new
        // one, which is born with SLink::defaultTimebaseFor()'s timebase and
        // no channel override. Captured as the link's own types -- rounding
        // startTicks through an integer here would move a beats-timebase clip
        // on every undo, which is why merge-columns' Operand keeps a Fraction
        // too.
        pinnedLink_.timebase             = clipLink->getTimebase();
        pinnedLink_.startTicks           = clipLink->getStartTicks();
        pinnedLink_.eventChannelOverride = clipLink->getEventChannelOverride();

        // STrack::trackChildWasAdded wires TWO object-level signals --
        // durationChanged, and eventsChanged for Event-kind content -- and
        // trackChildWasRemoved severs neither, nor are those connects
        // Qt::UniqueConnection. Re-linking without this doubles them.
        //
        // The BROAD form on purpose: stakes::wrapCutLinkIntoStack's narrow
        // spelling names durationChanged only and would miss eventsChanged on
        // a MIDI take column. Nothing connects lane -> object, so this severs
        // exactly those two, and re-linking re-makes both through setParent ->
        // childEvent -> childObjectAdded -> trackChildWasAdded. That is why
        // SRestoreTrackAction re-connects nothing explicitly either.
        QObject::disconnect( obj, nullptr, track, nullptr );
    }

    delete clipLink;  // Qt will remove from parent, SCut destructor handles cleanup

    // A pinned column is restored by re-linking the very object that was
    // deleted, so every field survives by identity -- the comp map, each
    // take's fade and name, the wrapper's parameters, the link's timebase.
    // Checked before the two rebuild-from-parameters branches because it is
    // strictly more faithful than either.
    if( canPin ) {
        return {true, new SRestorePinnedClipAction( this, trackPath_, timePos_,
                                                    clipIndex_ )};
    }

    // Hand the inverse the index the clip CAME FROM (QBX-149). Without it the
    // restored clip is appended, so every later positional clip index means a
    // different clip -- and a redo of this very delete destroys the wrong one.
    if( canRestoreContainer ) {
        return {true, new SRestoreContainerClipAction(
                          trackPath_, containerPath, timePos_,
                          srcStart, cutDuration, loopLength, grain,
                          clipIndex_ )};
    }

    SAddSampleAction *inverse =
        haveWindow
            ? new SAddSampleAction( trackPath_, filePath, timePos_,
                                    srcStart, cutDuration, loopLength, grain,
                                    clipIndex_ )
            : new SAddSampleAction( trackPath_, filePath, timePos_,
                                    clipIndex_ );
    return {true, inverse};
}

void SRemoveSampleAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackPath", strackpath::qualifiedToString( pathRoot_, trackPath_ ));
    elem.setAttribute("clipIndex", clipIndex_);
    elem.setAttribute("filePath", filePath_);
    elem.setAttribute("timePos", QString::fromStdString(Fraction(timePos_, 1).toString()));
}

bool SRemoveSampleAction::readXml(const QDomElement &elem, int /*version*/)
{
    // Sniff the spelling rather than key off formatVersion(): pre-existing .qxa
    // scripts carry no version attribute, and `trackIndex` is exactly a
    // one-element path.
    trackPath_ = elem.hasAttribute("trackPath")
        ? strackpath::parseInto( pathRoot_, elem.attribute("trackPath") )
        : QList<int>{ elem.attribute("trackIndex", "0").toInt() };
    clipIndex_ = elem.attribute("clipIndex", "0").toInt();
    filePath_ = elem.attribute("filePath", "");
    timePos_ = (offset_t)parseFractionOrDouble(elem.attribute("timePos", "0").toStdString()).toDouble();
    return true;
}

static const bool s_reg_removesample = (
    SActionRegistry::instance().registerType(
        QStringLiteral("remove-sample"),
        []{ return new SRemoveSampleAction; }
    ), true
);
