#include "app/testkit/slogstressaction.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"

#include "tw/core/twlog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDomElement>
#include <QElapsedTimer>
#include <QThread>
#include <QWidget>

#include <algorithm>

SApplyResult SLogStressAction::apply(SProject * /*project*/)
{
    // Same lookup the drag-clip-edge action uses: in test mode the window is
    // built but never shown, which is fine here — the model's drain is driven
    // by its timer and by processEvents, not by screen geometry.
    SMainWindow *win = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if ((win = qobject_cast<SMainWindow*>(w))) break;
    }
    if (!win) {
        TW_LOGE("ui.testkit", "log-stress: no main window");
        return {false, nullptr};
    }

    win->setLogDockVisible(true);
    QCoreApplication::processEvents();

    // Force the sink to accept our records regardless of how the run was
    // configured. Without this the case silently passes while doing NOTHING:
    // at --log-level=info every TW_LOGD below is gated out at the call site,
    // the ring never grows, and the drain has no work to prove itself on.
    tw::TwLog &log = tw::TwLog::instance();
    const tw::LogLevel savedLevel = log.minLevel();
    const bool savedConsole = log.console();
    log.setMinLevel(tw::LogLevel::Debug);
    log.setConsole(false);   // teeing 300k lines to stderr would dwarf the test

    const uint64_t seqBefore = log.nextSeq();
    win->logViewResetDrainStats();

    // Emit the burst. These go through the same path as any other diagnostic,
    // so the ring, the console tee and the dock all see them.
    for (int i = 0; i < count_; ++i) {
        TW_LOGD("stress", "log-stress record %d of %d: filler payload to give "
                          "the row a realistic width", i, count_);
    }

    const uint64_t emitted = log.nextSeq() - seqBefore;
    log.setConsole(savedConsole);

    // Now pump, timing every pump. A drain that blocks the GUI thread shows up
    // here and nowhere else.
    QElapsedTimer total;
    total.start();
    qint64 worstStallMs = 0;
    int pumps = 0;

    while (total.elapsed() < timeoutMs_) {
        QElapsedTimer pump;
        pump.start();
        QCoreApplication::processEvents();
        const qint64 ms = pump.elapsed();
        if (ms > worstStallMs) worstStallMs = ms;
        ++pumps;

        if (win->logViewBacklog() == 0 && pumps > 5) break;

        // The drain is on a 100 ms timer; without this the loop is a hot spin
        // between ticks, which says nothing and burns a core.
        QThread::msleep(1);
    }

    const int backlog = win->logViewBacklog();
    const int rows    = win->logViewRows();
    const qint64 elapsed = total.elapsed();
    const qint64 worstDrainMs = win->logViewWorstDrainMs();
    log.setMinLevel(savedLevel);

    // What the view can hold is min(emitted, ring capacity) -- the ring evicts
    // first, then the view caps at the ring's size.
    const uint64_t expectRows =
        std::min<uint64_t>(emitted, (uint64_t)log.capacity());

    TW_LOGI("ui.testkit",
            "log-stress: emitted %llu, %d rows displayed (expected ~%llu), "
            "backlog %d, %d pumps in %lld ms, worst drain tick %lld ms "
            "(worst whole pump %lld ms, includes unrelated app work)",
            (unsigned long long)emitted, rows, (unsigned long long)expectRows,
            backlog, pumps, (long long)elapsed, (long long)worstDrainMs,
            (long long)worstStallMs);

    // Guard against the test passing by doing nothing.
    if (emitted < (uint64_t)count_) {
        TW_LOGE("ui.testkit",
                "log-stress: only %llu of %d records reached the ring",
                (unsigned long long)emitted, count_);
        return {false, nullptr};
    }
    if ((uint64_t)rows * 10 < expectRows * 9) {
        TW_LOGE("ui.testkit",
                "log-stress: view holds %d rows, expected about %llu",
                rows, (unsigned long long)expectRows);
        return {false, nullptr};
    }

    if (backlog != 0) {
        TW_LOGE("ui.testkit",
                "log-stress: view did not catch up within %d ms (backlog %d)",
                timeoutMs_, backlog);
        return {false, nullptr};
    }
    // Assert on the DRAIN, not on the whole pump. A pump also carries whatever
    // else the app had queued -- the startup device-latency probe alone costs
    // ~90 ms here, and an earlier version of this action failed on that,
    // blaming the dock for work it had nothing to do with. Confirmed by running
    // the same case with 200 records: same ~90 ms pump, no log traffic at all.
    if (worstDrainMs > maxStallMs_) {
        TW_LOGE("ui.testkit",
                "log-stress: a single drain tick took %lld ms (limit %d) "
                "-- the log view is blocking the GUI thread",
                (long long)worstDrainMs, maxStallMs_);
        return {false, nullptr};
    }
    return {true, nullptr};
}

void SLogStressAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("count", QString::number(count_));
    elem.setAttribute("maxStallMs", QString::number(maxStallMs_));
    elem.setAttribute("timeoutMs", QString::number(timeoutMs_));
}

bool SLogStressAction::readXml(const QDomElement &elem, int /*version*/)
{
    count_      = elem.attribute("count", "100000").toInt();
    maxStallMs_ = elem.attribute("maxStallMs", "50").toInt();
    timeoutMs_  = elem.attribute("timeoutMs", "60000").toInt();
    return true;
}

static const bool s_reg_log_stress = (
    SActionRegistry::instance().registerType(
        QStringLiteral("log-stress"),
        []{ return new SLogStressAction; }
    ), true
);
