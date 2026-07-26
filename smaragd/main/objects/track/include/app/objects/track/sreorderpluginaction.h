#ifndef SREORDERPLUGINACTION_H
#define SREORDERPLUGINACTION_H

#include "app/actions/saction.h"

// Action: move one plugin slot to another position in a track's chain.
//
// proposal 08 M5. The FX strip's drop handler used to call
// SPluginChain::reorderSlot() DIRECTLY (splugineffectstrip.cpp), so a
// drag-to-reorder was neither undoable nor scriptable — the same
// invariant-3 violation as the bypass checkbox.
//
// Inverse: reorder-plugin the other way round. That is exact because
// SObject::moveChildToIndex() is QList::move(), whose inverse is
// move(toIndex, fromIndex) — not a swap.
class SReorderPluginAction : public SAction {
public:
    SReorderPluginAction( const QString &trackPath = QString(),
                          int fromIndex = 0, int toIndex = 0 );

    QString name() const override { return QStringLiteral( "reorder-plugin" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     fromIndex_;
    int     toIndex_;
};

#endif  // SREORDERPLUGINACTION_H
