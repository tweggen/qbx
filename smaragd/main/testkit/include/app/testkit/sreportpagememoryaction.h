#ifndef SREPORTPAGEMEMORYACTION_H
#define SREPORTPAGEMEMORYACTION_H

#include "app/actions/saction.h"

/**
 * Test hook: report resident frozen-page memory (proposal 36 B1a).
 *
 * There is no twOutputPage pool to query — pages are make_shared on demand into
 * unbounded per-component maps — so "how much page memory does this project
 * hold" had no answer at all. B1a builds the accounting
 * (tw::pages::PageAccounting for the global figures, twComponent::pageStats for
 * the per-component breakdown); this verb is how a .qxa asks for it, which is
 * what makes the corpus measurable rather than merely renderable.
 *
 * It logs, at Info level under category "pages":
 *
 *   page-memory [label] global=N pages/B bytes inComponents=… frozen=…
 *                       elsewhere=… components=… everAllocated=… peakBytes=…
 *   page-memory [label]   <type> instances=… holders=… pages=… frozen=… bytes=…
 *
 * `elsewhere` is `global - inComponents`: pages that are alive but no longer in
 * any component's cache — bound into a scheduler node, held by an audio
 * callback, or chained as a stalePredecessor. It is a real number, not a
 * rounding error, and a phase that "frees" pages by dropping them from a map
 * while something still holds them shows up there rather than as a saving.
 *
 * XML format:
 * <report-page-memory label="after render" maxPages="-1" maxBytes="-1"/>
 *
 * Parameters:
 * - label:    names the moment in the log line (default "")
 * - maxPages: reject if more than this many pages are resident GLOBALLY
 *             (default -1 = report only, assert nothing)
 * - maxBytes: same for resident sample bytes (default -1)
 *
 * The two bounds default off on purpose. A page count is a function of the
 * readahead, the worker count and the scheduler's timing, so a tight bound would
 * be a flake generator; the verb's job is to make the number VISIBLE, and a case
 * that wants a ceiling has to choose one it can defend.
 */
class SReportPageMemoryAction : public SAction {
public:
    SReportPageMemoryAction() = default;
    explicit SReportPageMemoryAction( const QString &label,
                                      long long maxPages = -1,
                                      long long maxBytes = -1 );

    QString name() const override { return QStringLiteral("report-page-memory"); }
    QStringList knownAttributes() const override {
        return {QStringLiteral("label"), QStringLiteral("maxPages"),
                QStringLiteral("maxBytes")};
    }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

private:
    QString   label_;
    long long maxPages_ = -1;
    long long maxBytes_ = -1;
};

#endif // SREPORTPAGEMEMORYACTION_H
