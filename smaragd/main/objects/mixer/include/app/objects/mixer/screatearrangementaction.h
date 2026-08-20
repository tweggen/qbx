#ifndef SCREATEARRANGEMENTACTION_H
#define SCREATEARRANGEMENTACTION_H

#include "app/actions/saction.h"
#include <QString>

// Action: create a named ARRANGEMENT — a second summing root (an SStdMixer)
// that is NOT the project's master root and is therefore NOT summed into it.
// Its audio reaches the master only through an asset placed over it, so it is
// heard exactly once (proposal 09 D8) and cannot be placed inside its own
// container the way an in-place asset can.
//
// The root is registered on the project by name, which pins one reference —
// without it a root that nothing places is a top-level object nobody owns and
// the loader collects it. See SProject::registerArrangement.
//
// Inverse: SRemoveArrangementAction.
class SCreateArrangementAction : public SAction {
public:
    SCreateArrangementAction() = default;
    explicit SCreateArrangementAction( const QString &name );

    QString name() const override { return QStringLiteral("create-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override { return { QStringLiteral("name") }; }

private:
    QString name_;   // empty => generate a unique one at apply()
};

#endif // SCREATEARRANGEMENTACTION_H
