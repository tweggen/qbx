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

private:
    QList<int> sourceClipPath_;   // [track path..., link index] of the original
    QList<int> destTrackPath_;    // path TO the destination track
    offset_t   startTime_ = 0;
};

#endif // SDUPLICATECLIPACTION_H
