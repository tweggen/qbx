#ifndef SDELETECLIPACTION_H
#define SDELETECLIPACTION_H

#include "../saction.h"
#include "tw303aenv.h"
#include "twgrainparams.h"
#include <QList>

class SProject;
class SObject;

// Delete the clip at clipPath (the undoable form of the Delete key / "Remove
// sample"). Unlike SRemoveClipAction (the inverse of a duplicate, which re-runs
// the duplicate to undo), this works for ANY clip: its inverse, SRecreateClipAction,
// carries a full snapshot of the removed clip and rebuilds it from scratch.
//
// Path-based and re-snapshots the live clip on every apply, so it survives redo
// (after an undo recreated the clip at the same spot) and replay-from-XML.
class SDeleteClipAction : public SAction {
public:
    SDeleteClipAction() = default;
    explicit SDeleteClipAction( const QList<int> &clipPath );

    QString name() const override { return QStringLiteral("delete-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;   // [track path..., link index] of the clip to delete
};

// Inverse of SDeleteClipAction: rebuild a clip that was deleted, from a snapshot.
// Created live only (it holds a raw pointer to the shared content), so it is not
// registered/serialized — mirrors SRemoveClipAction. The content is PINNED with
// addRef() for the whole lifetime of this action, so it survives even when the
// delete removed the content's last placement; the pin is released in the dtor.
class SRecreateClipAction : public SAction {
public:
    // SCut form: rebuild an SCut over `content` with the given window. insertIndex
    // is the child slot the clip was deleted from; the recreate restores it there
    // (not appended) so absolute index paths stay valid across undo/redo.
    SRecreateClipAction( const QList<int> &destTrackPath, int insertIndex,
                         SObject *content, offset_t startTime, offset_t startOffset,
                         length_t duration, length_t loopLength,
                         const twGrainParams &grain );
    // Raw-link form: rebuild a bare SLink wrapping `content`.
    SRecreateClipAction( const QList<int> &destTrackPath, int insertIndex,
                         SObject *content, offset_t startTime );
    ~SRecreateClipAction() override;

    QString name() const override { return QStringLiteral("recreate-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int>    destTrackPath_;       // path TO the track the clip lived on
    int           insertIndex_ = -1;    // child slot to restore the clip at
    SObject      *content_ = nullptr;   // shared content, pinned via addRef()
    bool          isCut_   = false;
    offset_t      startTime_   = 0;
    offset_t      startOffset_ = 0;
    length_t      duration_    = 0;
    length_t      loopLength_  = 0;
    twGrainParams grain_;
};

#endif // SDELETECLIPACTION_H
