#ifndef SQTFREETESTACTIONS_H
#define SQTFREETESTACTIONS_H

#include "app/actions/saction.h"

#include <QString>
#include <QStringList>

// watch-qt-free / assert-qt-free — QBX-305, docs/contracts/THREADING.md rule 1.
//
// A raw std::thread that claims to be Qt-free opens a twQtFreeScope
// (tw/core/twqtfree.h) for its work. watch-qt-free installs the engine's
// QObject-construction hook and zeroes its counters; assert-qt-free then reads
// how many QObjects were constructed inside an open scope since. Adoption is
// one of them: Qt adopts a thread by constructing a QAdoptedThread ON it, so a
// nonzero count means a worker was (or could have been) adopted.
//
// TEST VERBS: not undoable, write nothing into the project.
//
// watch-qt-free: no attributes. REJECTED when this Qt offers no hook table.
// assert-qt-free: `maxViolations` = "0", `minScopes` = "1" (at least this many
// scopes opened since the watch: a worker that lost its scope would otherwise
// pass by never looking). FAILS on either bound; logs both numbers.

class SWatchQtFreeAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "watch-qt-free" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;
};

class SAssertQtFreeAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-qt-free" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    qint64 maxViolations_ = 0;
    qint64 minScopes_     = 1;
};

#endif
