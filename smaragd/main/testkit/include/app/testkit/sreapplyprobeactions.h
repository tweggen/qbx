#ifndef _SREAPPLYPROBEACTIONS_H_
#define _SREAPPLYPROBEACTIONS_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * QBX-276: the test verbs that gate WHICH inverse an undo runs after a redo.
 *
 * `probe-reapply` changes nothing in the project. Each apply bumps its own
 * application count and returns an inverse stamped with that count, holding
 * the forward by a raw pointer: the same borrowing shape a held plan's
 * `unmerge-columns` has, minus the freed memory. When the inverse runs it
 * compares its stamp with the forward's current count. A mismatch means the
 * undo ran the inverse of an application a redo has since SUPERSEDED, and is
 * counted, process-wide.
 *
 * `assert-reapply-probe stale= undos=` checks those two counts: inverses
 * that ran against a superseded application, and inverses that ran at all.
 * The second keeps the first honest: a probe whose undo never ran also
 * reads stale="0".
 *
 * Both answers are integers and pointer identities, never a dereference of
 * something possibly freed. That is why this gates the protocol in an
 * ordinary build, where the real verbs' use-after-free is usually hidden by
 * the allocator handing the freed block straight back to the re-plan.
 */
class SProbeReapplyAction : public SAction
{
public:
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "probe-reapply" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    int applications() const { return applications_; }

    static int staleUndos();
    static int undos();
    static void noteUndo( bool stale );

private:
    int applications_ = 0;
};

class SAssertReapplyProbeAction : public SAction
{
public:
    SApplyResult apply( SProject *project ) override;
    QString name() const override { return QStringLiteral( "assert-reapply-probe" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral( "stale" ), QStringLiteral( "undos" ) }; }

private:
    int stale_ = 0;
    int undos_ = -1;     // < 0 = not checked
};

#endif
