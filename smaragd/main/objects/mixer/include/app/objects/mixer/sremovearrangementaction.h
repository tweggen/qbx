#ifndef SREMOVEARRANGEMENTACTION_H
#define SREMOVEARRANGEMENTACTION_H

#include "app/actions/saction.h"
#include <QString>

// Action: unregister a named arrangement root, dropping the registry's pin.
//
// REFUSED while any registered asset still windows that root. The refusal is
// the interesting part: unregistering would drop the only reference a detached
// root has, and an asset windowing a dead container is not a recoverable state
// — its undo could not rebuild the arrangement's contents. An asset must be
// removed first (or the whole thing dissolved), which is exactly what
// dissolve-arrangement will do as one macro.
//
// Inverse: SCreateArrangementAction with the same name. That inverse rebuilds
// an EMPTY root, which is correct only because the refusal above means a
// removable arrangement is one nothing references — a populated one is
// unreachable through this verb.
class SRemoveArrangementAction : public SAction {
public:
    SRemoveArrangementAction() = default;
    explicit SRemoveArrangementAction( const QString &name );

    QString name() const override { return QStringLiteral("remove-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override { return { QStringLiteral("name") }; }

private:
    QString name_;
};

#endif // SREMOVEARRANGEMENTACTION_H
