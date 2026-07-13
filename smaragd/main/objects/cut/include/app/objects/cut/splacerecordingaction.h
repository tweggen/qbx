#ifndef SPLACERECORDINGACTION_H
#define SPLACERECORDINGACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

// Action: place a finished recording onto a track — the multi-take verb
// (proposal 17 phase 2, decision 1). Plans the recording's span against the
// lane's existing columns and applies the plan as one atomic composite:
//
//   - a column (take stack or plain cut) the recording covers gets the
//     matching segment as a NEW take (slip = columnStart - recording start),
//     auto-activated; plain cuts are wrapped into stacks by add-take,
//   - uncovered spans become plain windowed cuts (place-clip),
//   - columns that begin BEFORE the recording start are left untouched
//     ("as applicable").
//
// An empty lane region therefore degenerates to today's behavior: one plain
// cut over the whole file. Undo removes everything the recording placed and
// unwraps any wrapped columns (composite of the children's inverses).
class SPlaceRecordingAction : public SAction {
public:
    SPlaceRecordingAction() = default;
    SPlaceRecordingAction( const QList<int> &trackPath,
                           const QString &filePath, offset_t timePos );

    QString name() const override { return QStringLiteral("place-recording"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    filePath_;
    offset_t   timePos_ = 0;
};

#endif // SPLACERECORDINGACTION_H
