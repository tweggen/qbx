#ifndef SSETPROPERTYACTION_H
#define SSETPROPERTYACTION_H

#include "../saction.h"
#include <QVariant>

// Generic action: set an arbitrary key/value on the project's property dict.
// The named toggle actions (snap-to-grid, grid, ...) are the convenience
// wrappers; this is the general-purpose verb the scripting layer would use.
// Non-undoable for now (these are transient settings).
class SSetPropertyAction : public SAction {
public:
    SSetPropertyAction() = default;
    SSetPropertyAction( const QString &key, const QVariant &value );

    QString name() const override { return QStringLiteral("set-property"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  key_;
    QVariant value_;
};

#endif // SSETPROPERTYACTION_H
