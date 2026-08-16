#ifndef _TW_LEVEL_PROBE_H_
#define _TW_LEVEL_PROBE_H_

// Proposal 34 — reads levels out of a component's FROZEN PAGES by position.
//
// This is the whole trick of the feature. Pages are position-keyed: a page
// frozen four seconds ahead of the playhead still DESCRIBES the audio at the
// position it covers, so "scan the page that covers the playhead" is inherently
// "what is audible", no matter which worker thread froze it or how far ahead.
// Computing levels at freeze time instead would show the future.
//
// The probe never blocks, never waits, and never creates a demand: it reads the
// component's page cache through getPageIfExists() (a try-lock lookup that
// returns nullptr rather than stalling) and reports a miss otherwise. A miss is
// the caller's cue to decay the meter, so a dropout reads as a fast fall.
//
// Threading: ONE INSTANCE PER METER, used from ONE thread (in practice the UI
// thread). It is not itself thread-safe, and it must NEVER be used from the RT
// audio callback — not because it would be unsafe, but because there is no
// reason to: the pump already runs on the main thread.

#include "tw/graph/twcomponent.h"
#include "tw/metering/tw_level_scan.h"
#include "tw/pages/tw_output_page.h"

#include <memory>

class twLevelProbe
{
public:
    // Longest span one advanceTo() will scan (100 ms at 48 kHz). When the pump
    // has been starved, or the position jumped forward, the window is the most
    // recent MAX_WINDOW frames — capping the work without discarding the frames
    // that are about to be heard.
    static constexpr length_t MAX_WINDOW = 4800;

    // Span used when the position did NOT advance: no history yet (a first tick
    // after reset), transport stopped, a seek backwards, a loop wrap.
    static constexpr length_t MIN_WINDOW = 256;

    // The component whose frozen pages are measured. For a track that is
    // STrack::getRootComponent() (the per-track twRewire): post-fader, post-FX,
    // pre-summing, and the only per-track component that actually CACHES pages
    // — twTrackMix allocates a fresh page per call and twPluginChain forwards,
    // so neither can be probed.
    void setTap( std::shared_ptr<twComponent> tap );
    const std::shared_ptr<twComponent> &tap() const { return tap_; }

    // Forget the position history and the held page. Call on transport start, on
    // a seek, or whenever the tap changes meaning.
    void reset();

    // Measure the span that ends at `pos` and begins where the last call ended
    // (clamped to [MIN_WINDOW, MAX_WINDOW]). Returns false on a miss — no tap,
    // nothing elapsed, or no frozen page covering the span — in which case
    // `out` is left with frames == 0.
    bool advanceTo( offset_t pos, twLevelSample &out,
                    float clipThreshold = TW_METER_CLIP_THRESHOLD );

    // The N-lane form (proposal 36 B8). Measures the SAME span, once per lane,
    // and reports how many lanes it actually found:
    //
    //     out.lanes = min( wantLanes, page->channels(), MAX_LANES )
    //
    // ACT ON THE WIDTH OF THE PAGE IN HAND (§4.4), never on a declared width:
    // an insert-less twPluginChain forwards its input page verbatim and its
    // silence pages are default-constructed width 1, so a caller asking for six
    // lanes can legitimately be handed one. A caller that wants a stable lane
    // count must clamp the meter it draws, not this. (The producer's DECLARED
    // width is still consulted, in resolvePage_, but for a different question:
    // §4.5's "a page whose width no longer matches its producer is a MISS".)
    //
    // wantLanes <= 0 is read as 1. The scalar overload above is exactly this
    // with wantLanes == 1, and both share one window/page/clamp computation, so
    // a scalar caller can never disagree with a lane-0 reading.
    bool advanceTo( offset_t pos, twLevelSampleSet &out, int wantLanes,
                    float clipThreshold = TW_METER_CLIP_THRESHOLD );

    // Diagnostics: how many advanceTo() calls found no page to read.
    uint64_t missCount() const { return misses_; }

private:
    // The acceptance ladder — a read-only echo of AudioEngine::updateFrozenPage,
    // so the meter shows the audio the ear gets rather than the audio the graph
    // wishes it were producing. Returns nullptr when nothing is readable.
    std::shared_ptr<twOutputPage> resolvePage_( offset_t pageStart );

    std::shared_ptr<twComponent>  tap_;
    std::shared_ptr<twOutputPage> held_;      // kept alive between ticks
    offset_t                      lastPos_ = 0;
    bool                          havePos_ = false;
    uint64_t                      misses_  = 0;
};

#endif  // _TW_LEVEL_PROBE_H_
