#ifndef STABTESTACTIONS_H
#define STABTESTACTIONS_H

#include "app/actions/saction.h"
#include <QString>

// <assert-tab-set names="Arrangement" active="Arrangement"/>
//
// The view shell's tab labels, IN ORDER, and which one is active
// (proposal 09 §2). Order matters here, unlike assert-arrangements: the whole
// contract of the shell is that the master is tab 0 and stays there.
//
// `names` may be given as empty to assert there are no tabs at all; `active`
// is checked only when non-empty.
class SAssertTabSetAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-tab-set" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("names"), QStringLiteral("active") }; }

private:
    QString names_;
    QString active_;
    bool    namesGiven_ = false;
};

#endif  // STABTESTACTIONS_H
