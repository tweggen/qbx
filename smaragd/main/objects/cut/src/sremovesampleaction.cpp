#include "app/objects/cut/sremovesampleaction.h"
#include "app/model/splacements.h"
#include "app/objects/cut/saddsampleaction.h"
#include "app/objects/cut/srestorecontainerclipaction.h"
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

SApplyResult SRemoveSampleAction::apply(SProject *project)
{
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

    if( !canRestoreContainer && !canRestoreFile ) {
        // REFUSE, and say so. A refusal the user cannot see is the same bug
        // wearing a different face, and this project's rule is that any
        // refusal is announced rather than silent.
        //
        // This is deliberately the CONSERVATIVE half of the fix: nothing is
        // lost, at the cost of a delete the user has to work around. Giving
        // these shapes a real inverse -- a restore-take-column action
        // carrying every take and the selected index -- is the follow-up.
        const SObject *content = nullptr;
        if( SClipWindow *w = SClipWindow::of( &clipLink->getSObject() ) ) {
            content = &w->windowContent();
        }
        TW_LOGW( "cut",
                 "remove-sample: refusing to delete clip %d on '%s' -- its "
                 "content (%s) is neither file-backed nor a path container, so "
                 "the deletion could not be undone. QBX-146.",
                 clipIndex_,
                 strackpath::qualifiedToString( pathRoot_, trackPath_ )
                     .toUtf8().constData(),
                 content ? content->metaObject()->className() : "unknown" );
        return {false, nullptr};
    }

    delete clipLink;  // Qt will remove from parent, SCut destructor handles cleanup

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
