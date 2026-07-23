#include "app/objects/cut/sremovesampleaction.h"
#include "app/model/splacements.h"
#include "app/objects/cut/saddsampleaction.h"
#include "app/objects/cut/srestorecontainerclipaction.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/cut/scut.h"
#include "app/model/slink.h"
#include "app/model/sexternfile.h"
#include "tw/core/twfraction.h"
#include <QDomElement>

SRemoveSampleAction::SRemoveSampleAction(int trackIdx, int clipIdx, const QString &filePath, offset_t timePos)
    : trackIndex_(trackIdx), clipIndex_(clipIdx), filePath_(filePath), timePos_(timePos)
{
}

SApplyResult SRemoveSampleAction::apply(SProject *project)
{
    if (!project) {
        return {false, nullptr};
    }

    SObject *root = splacements::rootContainer( project );
    if (!root || !root->isPathContainer()) {
        return {false, nullptr};
    }

    // Get the track.
    SLink *trackLink = root->childAt(trackIndex_);
    if (!trackLink) {
        return {false, nullptr};
    }

    SObject *track = &trackLink->getSObject();
    if (!track->isPathContainer()) {
        return {false, nullptr};   // addressed child is not a lane
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

    if( SCut *cut = dynamic_cast<SCut *>( &clipLink->getSObject() ) ) {
        SObject &content = cut->getContent();
        if( SExternFile *xf = dynamic_cast<SExternFile *>( &content ) ) {
            filePath = xf->getFileName();
            // The whole window, so undo restores the clip the user actually had
            // — not a default full-length one. Blocking duration read (P19):
            // the try-lock snapshot can hand back a stale pre-edit value.
            srcStart    = cut->getSrcStart();
            cutDuration = cut->getDurationBlocking();
            loopLength  = cut->getLoopLength().frames();
            grain       = cut->getGrainParams();
            haveWindow  = true;
        } else if( content.isPathContainer() ) {
            // An asset COPY: a cut windowing a track. Nothing in the registry
            // points at it, so undo has to rebuild the cut over the same
            // container. (A placement of the asset BODY itself never reaches
            // this action — the view routes it to SRemoveAssetPlacementAction,
            // whose inverse re-places the body and so keeps asset identity.)
            containerPath = strackpath::pathOf( root, &content );
            srcStart    = cut->getSrcStart();
            cutDuration = cut->getDurationBlocking();
            loopLength  = cut->getLoopLength().frames();
            grain       = cut->getGrainParams();
            haveWindow  = true;
        }
    }

    // The lane we are removing from, as a path, for the container-backed
    // inverse (which must also work for a nested lane).
    QList<int> lanePath = strackpath::pathOf( root, track );

    delete clipLink;  // Qt will remove from parent, SCut destructor handles cleanup

    if( !containerPath.isEmpty() && haveWindow ) {
        return {true, new SRestoreContainerClipAction(
                          lanePath, containerPath, timePos_,
                          srcStart, cutDuration, loopLength, grain )};
    }

    // Neither file-backed nor container-backed: removed, but nothing can
    // rebuild it. Report no inverse so the step is marked non-undoable rather
    // than failing loudly at undo time.
    if( filePath.isEmpty() ) {
        return {true, nullptr};
    }

    SAddSampleAction *inverse =
        haveWindow
            ? new SAddSampleAction( trackIndex_, filePath, timePos_,
                                    srcStart, cutDuration, loopLength, grain )
            : new SAddSampleAction( trackIndex_, filePath, timePos_ );
    return {true, inverse};
}

void SRemoveSampleAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("trackIndex", trackIndex_);
    elem.setAttribute("clipIndex", clipIndex_);
    elem.setAttribute("filePath", filePath_);
    elem.setAttribute("timePos", QString::fromStdString(Fraction(timePos_, 1).toString()));
}

bool SRemoveSampleAction::readXml(const QDomElement &elem, int /*version*/)
{
    trackIndex_ = elem.attribute("trackIndex", "0").toInt();
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
