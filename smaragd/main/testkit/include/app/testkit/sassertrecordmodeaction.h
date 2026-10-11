#ifndef SASSERTRECORDMODEACTION_H
#define SASSERTRECORDMODEACTION_H

#include "app/actions/saction.h"
#include <QList>
#include <QString>

/**
 * Assert the record mode in force for a lane (plan 53 §5.2, QBX-156 M0).
 *
 * `resolved` is the interesting one: it runs srecmode::resolveMode, so it sees
 * the whole rule -- the lane's own attribute, else the nearest ancestor FOLDER
 * track's, else the project's property, else the built-in default. That is the
 * only way a case can tell inheritance from coincidence.
 *
 * `own` asserts the lane's OWN attribute, where empty means inherit, and
 * `project` asserts the project property alone. Both exist because `resolved`
 * cannot distinguish "this lane states takes" from "this lane inherits takes
 * from a project that states it" -- and the difference is exactly what a
 * resolution case is about. Each is checked only when given.
 *
 * Not undoable.
 */
class SAssertRecordModeAction : public SAction {
public:
    SAssertRecordModeAction() = default;

    QString name() const override
    { return QStringLiteral("assert-record-mode"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    resolved_;
    QString    own_;
    QString    project_;
    bool       haveOwn_ = false;
};

#endif // SASSERTRECORDMODEACTION_H
