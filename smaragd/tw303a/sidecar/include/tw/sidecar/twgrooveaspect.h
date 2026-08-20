#ifndef _TWGROOVEASPECT_H_
#define _TWGROOVEASPECT_H_

#include <cstdint>
#include <vector>

#include "tw/sidecar/twgroove.h"
#include "tw/sidecar/twgroovependulum.h"

/**
 * Proposal 40 "Feel Flow" M1 -- packages twgroovependulum.h's pendulum
 * estimator output into the two sidecar aspect payloads ("groove.res" /
 * "groove.ev", tw/sidecar/twaspects.h) plus their shared params blob. Pure,
 * deterministic, single-threaded: no engine state, no I/O, no Qt, no
 * threads, no thread_local, no rand(). This is the wrapper a caller (app
 * code -- SPlainWave::enqueueGrooveAnalysis -- and this module's own round-
 * trip gate, sidecar_test.cc) uses to talk to twQafWriter/twQafReader;
 * nothing here changes the M0 estimator's algorithm (twGrooveAnalyzeFrontEnd
 * / twGroovePendulumAnalyze) -- it only reads their already-computed output
 * and reshapes it into the wire format twaspects.h documents.
 */

/**
 * Every ANALYSIS-SIDE free parameter that can change the resulting bytes:
 * the front-end config (twGrooveFrontEndParams) plus the ensemble/pendulum
 * and stats-pooling config (twGroovePendulumParams, which already carries
 * twGrooveStatsParams). serialize() is the canonical LE params blob for QAF
 * keying -- field order is normative, matching the house convention
 * (tw/sidecar/twanalyzers.h's twOnsetParams::serialize et al.): changing
 * ANY default here mints a new store key.
 */
struct twGrooveAnalysisParams {
    twGrooveFrontEndParams frontEnd;
    twGroovePendulumParams pendulum;

    void serialize( std::vector<uint8_t> &out ) const;
};

/** One decoded "groove.res" record (twaspects.h): per-unit normalized
 * resonance power (ensemble order) plus the compliance scalar. */
struct twGrooveResRecord {
    std::vector<float> unitPower;             // size nUnits, each in [0,1]
    float               compliance = 0.0f;    // in [0,1]
};

/** One decoded "groove.ev" record (twaspects.h). */
struct twGrooveEvRecord {
    uint64_t pos        = 0;
    float    residualMs = 0.0f;
    float    confidence = 0.0f;
    uint16_t region      = 0;
    uint16_t flags       = 0;
};

/**
 * The two aspect payloads plus the geometry a twQafInfo needs, built from
 * one pendulum analysis run.
 */
struct twGrooveAspectPayloads {
    uint32_t              hopFrames      = 0;   // "groove.res" hop (== rate/100, AC 1)
    uint32_t              nUnits         = 0;   // ensemble size -- "groove.res" record width - 1
    std::vector<uint8_t>  resPayload;
    uint64_t              resRecordCount = 0;   // == ceil(nFrames / hopFrames)
    std::vector<uint8_t>  evPayload;
    uint64_t              evRecordCount  = 0;   // == number of pass-2 scored events (may be 0)
};

/**
 * Runs the front end (twGrooveAnalyzeFrontEnd) and the free-running pendulum
 * estimator (twGroovePendulumAnalyze) over `chans`/`nCh`/`nFrames` at `rate`,
 * then encodes both aspect payloads per twaspects.h's normative doc:
 *
 *  - "groove.res": resampled from the pendulum's own hop grid
 *    (twGroovePendulumResult::hopFrames, the front end's envRateHz) onto the
 *    aspect's fixed rate/100 grid by linear interpolation of each unit's
 *    |z(hop)| trajectory and of the confidence trajectory. Per-unit power is
 *    normalized by THAT UNIT's own peak |z|^2 over the whole run (0 when the
 *    unit never moved at all).
 *  - "groove.ev": one record per twGroovePendulumResult::scoredEvents entry
 *    (already the pass-2 RAW per-event residual, pre-pooling), in the same
 *    ascending order; confidence is nearest-hop-sampled at the event's
 *    position from the SAME confidence trajectory "groove.res" resamples.
 *
 * Returns a default-constructed (empty payloads, hopFrames/nUnits == 0)
 * result on invalid input (chans == nullptr, nCh == 0, nFrames == 0,
 * rate == 0) or when the front end/pendulum has no lock -- mirrors
 * twGroovePendulumAnalyze's own honest-empty rule: an unanalyzable file is a
 * valid, distinguishable outcome, not an error.
 */
twGrooveAspectPayloads twGrooveBuildAspectPayloads(
    const float *const *chans, uint32_t nCh, uint64_t nFrames, uint32_t rate,
    const twGrooveAnalysisParams &params );

/**
 * Decodes one "groove.res" payload (recordStride == (nUnits+1)*4) into
 * records. `nUnits` is not carried in the QAF header; a caller derives it
 * from the reader's own geometry (payloadLen / recordCount / 4 - 1) or
 * already knows it (the params it stored with). Returns empty on a
 * stride/size mismatch (nUnits == 0, or payloadLen not a multiple of the
 * derived stride).
 */
std::vector<twGrooveResRecord> twGrooveDecodeResPayload(
    const uint8_t *payload, uint64_t payloadLen, uint32_t nUnits );

/** Decodes one "groove.ev" payload (recordStride == 20) into records.
 * Returns empty on a size mismatch (payloadLen not a multiple of 20). */
std::vector<twGrooveEvRecord> twGrooveDecodeEvPayload(
    const uint8_t *payload, uint64_t payloadLen );

#endif
