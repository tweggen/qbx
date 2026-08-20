#ifndef SARRANGEMENTTABACTIONS_H
#define SARRANGEMENTTABACTIONS_H

#include "app/actions/saction.h"
#include <QString>

// Tab verbs (proposal 09 §2). None of them is undoable: opening, closing or
// switching a window is not an edit to the arrangement, which is the same split
// proposal 33 D2 makes for a plugin editor's window.

class SOpenArrangementTabAction : public SAction {
public:
    QString name() const override { return QStringLiteral("open-arrangement-tab"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override { return { QStringLiteral("name") }; }
private:
    QString arrName_;
};

class SCloseArrangementTabAction : public SAction {
public:
    QString name() const override { return QStringLiteral("close-arrangement-tab"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override { return { QStringLiteral("name") }; }
private:
    QString arrName_;
};

// Switch the ACTIVE tab by label. The active tab is what stimeline::submitActive
// roots a gesture at, so this is how a script drives the arranger in an
// arrangement rather than in the master.
class SActivateTabAction : public SAction {
public:
    QString name() const override { return QStringLiteral("activate-tab"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override { return { QStringLiteral("name") }; }
private:
    QString tabName_;
};

#endif  // SARRANGEMENTTABACTIONS_H
