// twOutputPage's out-of-line half (proposal 36 B1b).
//
// The page struct is otherwise header-only and deliberately stays that way: the
// hot accessors (channelPtr) must inline into the freeze and the audio callback.
// What lives here is only the reporting for the two things that must never be
// silent and must never be a Q_ASSERT (§7 trap 9 — Q_ASSERT_X is compiled out of
// the build everyone runs, so an assert-as-check is a check that never runs):
// an out-of-range channel index and a scratch resize of a wide page.
//
// Both report ONCE per process. A page-level mistake in a freeze loop would
// otherwise emit 65536 records and cost more than the bug.

#include "tw/pages/tw_output_page.h"

#include "tw/core/twlog.h"

std::uint16_t twOutputPage::clampChannels(std::uint16_t requested)
{
    if (requested >= 1) {
        return requested;
    }
    // A zero-width page has no channel 0 and would divide the geometry by zero.
    // There is no honest thing to do with it except refuse it, loudly, and hand
    // back the width every page in the tree has today.
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        TW_LOGE("pages",
                "twOutputPage: channels=0 requested; a page must have at least "
                "one channel. Allocating width 1. (reported once)");
    }
    return 1;
}

void twOutputPage::reportChannelOutOfRange(idx_t c, std::uint16_t channels)
{
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        TW_LOGE("pages",
                "twOutputPage::channelPtr(%d) on a %u-channel page: out of "
                "range. Serving channel 0 so this can never be an out-of-bounds "
                "read on the audio thread — but the caller is wrong. The plug "
                "clamp of proposal 36 §4.4 belongs in twStreamingLatch, not "
                "here. (reported once)",
                (int)c, (unsigned)channels);
    }
}

void twOutputPage::reportScratchOnWidePage(std::uint16_t channels)
{
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        TW_LOGE("pages",
                "twOutputPage::resizeMonoScratch on a %u-channel page: refused. "
                "The scratch path is a plain buffer with no stride and is legal "
                "at width 1 only; a wide component renders into its own page. "
                "(reported once)",
                (unsigned)channels);
    }
}
