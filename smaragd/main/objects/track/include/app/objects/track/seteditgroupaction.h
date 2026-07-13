#ifndef SSETEDITGROUPACTION_H
#define SSETEDITGROUPACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: assign a track (lane) to an edit group (proposal 17 phase 4).
// `group` = 0 removes it from its group. Arbitrary sets: any lane may join
// any id, regardless of nesting; the folder-track "G" button is just a UI
// shortcut that submits one of these per subtree lane.
//
// Inverse: set-edit-group with the previous id.
class SSetEditGroupAction : public SAction {
public:
    SSetEditGroupAction() = default;
    SSetEditGroupAction( const QList<int> &trackPath, int group );

    QString name() const override { return QStringLiteral("set-edit-group"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    int        group_ = 0;
};

#endif // SSETEDITGROUPACTION_H
