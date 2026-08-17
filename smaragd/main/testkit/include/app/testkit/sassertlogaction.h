#ifndef SASSERTLOGACTION_H
#define SASSERTLOGACTION_H

#include "app/actions/saction.h"
#include <cstdint>

/**
 * Assertion action: a line matching `contains` was logged.
 *
 * Some behaviour has no other observable: a recovery is EXACTLY the case where
 * the render still sounds right, so nothing about the audio can tell you
 * whether the loader repaired the project or the project never needed
 * repairing. The warning it printed is the evidence, and a --test-case run
 * writes NO log file (main.cpp skips the file sink deliberately, so a headless
 * suite cannot race over one file in the user's config dir). So this reads the
 * in-process `TwLog` RING, which every channel funnels into — TW_LOG*,
 * syslog() and Qt's qWarning/qDebug through the installed message handler.
 *
 * XML format:
 *   <assert-log contains="SFutureThing"/>
 *   <assert-log contains="missing.wav" minCount="1" maxCount="2" level="warn"/>
 *   <assert-log contains="twview: null component" maxCount="0"/>
 *
 * Parameters:
 * - contains: literal substring (not a regex) of the record's text.
 * - minCount: fewest matching records that must be there (default 1).
 * - maxCount: most that may be (-1 = no upper bound, the default). `maxCount`
 *             = "0" with `minCount` = "0" asserts a line is ABSENT — that is
 *             how a case pins "this warning no longer happens".
 * - level:    "" (any, default) or error|warn|info|debug|trace: only records
 *             AT that level count.
 *
 * THE WINDOW: records logged since the previous action STARTED (the runner
 * marks each boundary; consecutive assert-logs share one window, so two of
 * them examine the same action). Counting the whole run instead would make
 * "twice" depend on what came before, and a case that renders for a minute
 * would count another case's shape of the same message. Records that the ring
 * evicted are simply not there — which is why a --test-case run raises the
 * ring's capacity: a long render must not be able to push the line under test
 * out before the assertion reads it.
 *
 * Not undoable (an assertion never is).
 */
class SAssertLogAction : public SAction {
public:
    SAssertLogAction() = default;
    SAssertLogAction( const QString &contains, int minCount = 1,
                      int maxCount = -1, const QString &level = QString() );

    QString name() const override { return QStringLiteral("assert-log"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("contains"), QStringLiteral("minCount"),
                QStringLiteral("maxCount"), QStringLiteral("level")};
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    /**
     * Called by SActionRunner before it submits each action. Anything but
     * another assert-log moves the window forward, so an assertion always
     * examines the action that precedes it (and two assertions in a row
     * examine the same one).
     */
    static void beginAction( const QString &verb );
    /** First log sequence number in the current window. */
    static uint64_t windowStart();

private:
    QString contains_;
    int     minCount_ = 1;
    int     maxCount_ = -1;
    QString level_;
};

#endif
