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

// <assert-selection paths="Drums:0,0;Drums:0,1"/>
//
// The ACTIVE root's selection, as root-qualified index paths, SEMICOLON
// separated (a single path is already comma separated). An empty `paths`
// asserts the active root has nothing selected.
//
// It reads the ACTIVE list deliberately: proposal 09 §3's property is that the
// active tab's selection is the app-wide one, so a case switches tabs and
// asserts what the app would now act on.
class SAssertSelectionAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-selection" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override { return { QStringLiteral("paths") }; }
private:
    QString paths_;
};

#endif  // STABTESTACTIONS_H
