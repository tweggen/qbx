#ifndef SRENAMEASSETACTION_H
#define SRENAMEASSETACTION_H

#include "app/actions/saction.h"
#include <QString>

// rename-asset: give a registered asset a new name (proposal 56 §4.7, Q3;
// nassau-digital GATES Q3.4 needs one and qbx had none).
//
// The registry entry moves from `assetName` to `newName` and the body keeps
// its identity: every placement links the BODY, so nothing that places the
// asset changes, and neither does a sampler layer's asset link (an SLink to
// the body). The body's display name follows when it was the old asset name.
// Refuses an empty name, an unknown asset, a name another asset already has,
// and a rename to the same name (an empty step on the undo stack). The inverse
// is rename-asset itself, the other way round.
class SRenameAssetAction : public SAction {
public:
    SRenameAssetAction() = default;
    SRenameAssetAction( const QString &assetName, const QString &newName );

    QString name() const override { return QStringLiteral( "rename-asset" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString assetName_;
    QString newName_;
};

#endif // SRENAMEASSETACTION_H
