#ifndef SDUPLICATECLIPACTION_H
#define SDUPLICATECLIPACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>

class SProject;
class SLink;
class SObject;

// Create a new SCut that duplicates an existing clip (same content + window) and
// place it at startTime on a destination track. Both the live drag-preview and
// the action use makeDuplicateClip() so the copy is identical.
SLink *makeDuplicateClip( SProject *project, SObject &srcObj,
                          SObject *destLane, offset_t startTime );

// THE SHARING SIBLING (QBX-217): one more PLACEMENT of the very same object,
// which is what the arranger's "Add link" means and what makes it different
// from a duplicate - edit either placement and both change, because there is
// one object under both.
//
// A TAKE COLUMN IS STILL DEEP-COPIED, exactly as makeDuplicateClip does it
// (proposal 42 M1), and that is not an inconsistency: two placements of one
// `STakeStack` share its `activeTake_`, so comping either would comp both,
// which is indistinguishable from comping being broken. The rule belongs to
// the COLUMN, not to the gesture, so "share" cannot override it. Spelled out
// because "Add link" promising sharing and then not sharing a column is the
// kind of thing a reader assumes is a bug.
SLink *makeSharedLink( SProject *project, SObject &srcObj,
                       SObject *destLane, offset_t startTime );

// Action: duplicate the clip at sourceClipPath onto destTrackPath at startTime
// (the undoable form of a Ctrl-drag duplicate). Inverse: SRemoveClipAction,
// whose own inverse re-duplicates — mirrors SSplitClipAction/SUnsplitClipAction.
class SDuplicateClipAction : public SAction {
public:
    SDuplicateClipAction() = default;
    SDuplicateClipAction( const QList<int> &sourceClipPath,
                          const QList<int> &destTrackPath,
                          offset_t startTime );

    QString name() const override { return QStringLiteral("duplicate-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    // AC-a1: an optional out-param, written by apply() with the path of the
    // newly created copy (destTrackPath_ + the index it landed at). A caller
    // building a Ctrl-drag macro (sstdmixerview.cpp) reads THIS, never the
    // action pointer itself after submitting: on the REJECTED path the action
    // is deleted by SActionHistory before control returns, so a getter on the
    // action would be a use-after-free. Left untouched when apply() fails.
    void setCreatedPathOut( QList<int> *out ) { createdPathOut_ = out; }

private:
    QList<int> sourceClipPath_;   // [track path..., link index] of the original
    QList<int> destTrackPath_;    // path TO the destination track
    offset_t   startTime_ = 0;
    QList<int> *createdPathOut_ = nullptr;   // not owned; see setCreatedPathOut
};

// Action: place the clip at `source` AGAIN, as a shared link, on `destTrack`
// at `startTime` - the undoable form of the arranger's "Add &link" command
// (QBX-217). Inverse: SRemoveClipAction carrying share=true, so the REDO
// re-shares instead of re-duplicating.
//
// IT LIVES BESIDE SDuplicateClipAction DELIBERATELY. The two differ in one
// bit: everything else - resolving the source clip, refusing a lane link as a
// source, asking `placementLaneAt` for the destination (so proposal 45 D6
// applies here too), the take-column deep copy, the inverse - is the same, and
// the shared `makeSharedLink`/`makeDuplicateClip` pair is where the bit is
// spent. A separate .cpp would have needed its own CMakeLists entry as well,
// which is the one mistake in this repo a green build does not catch.
class SLinkClipAction : public SAction {
public:
    SLinkClipAction() = default;
    SLinkClipAction( const QList<int> &sourceClipPath,
                     const QList<int> &destTrackPath,
                     offset_t startTime );

    QString name() const override { return QStringLiteral("link-clip"); }
    QStringList knownAttributes() const override {
        return { QStringLiteral("source"), QStringLiteral("destTrack"),
                 QStringLiteral("startTime") };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    // As SDuplicateClipAction's: the path the new link landed at, for a caller
    // that needs it after submitting. Never read off the action itself - on
    // the rejected path SActionHistory has already deleted it.
    void setCreatedPathOut( QList<int> *out ) { createdPathOut_ = out; }

private:
    QList<int> sourceClipPath_;
    QList<int> destTrackPath_;
    offset_t   startTime_ = 0;
    QList<int> *createdPathOut_ = nullptr;
};

#endif // SDUPLICATECLIPACTION_H
