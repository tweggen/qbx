#ifndef _TWGROOVEPENDULUM_H_
#define _TWGROOVEPENDULUM_H_

#include <cstdint>
#include <string>
#include <vector>

#include "tw/sidecar/twgroove.h"

/**
 * The pendulum ensemble (proposal 40 "Feel Flow", M0 section 6 / design
 * section 3.2-3.5). Pure, deterministic, single-threaded: no engine state, no
 * I/O, no Qt, no threads, no thread_local, no rand(). Consumes ONLY a
 * twGrooveField (twgroove.h) -- no grid, no tempo map, matching the "the
 * reference is not a quantization grid" refinement.
 *
 * Build order per design section 3.2 ("trap 3"): LINEAR resonators (beta = 0
 * in the design's Hopf form) plus adaptive frequency, nothing more. The
 * nonlinear (beta != 0) term is explicitly deferred to a later milestone if
 * the linear tolerance behaviour proves too brittle -- it is NOT implemented
 * here.
 *
 * Units, matching the design's z/omega notation: z is complex (a
 * std::complex<double> pair per unit per hop), dt = 1/envRateHz (the SAME hop
 * rate the front end decimates region flux to -- field.hopFrames/field.rate).
 * Per hop:
 *
 *   F_p(t)     = sum_r  w_p(r) * field.regionFlux[r][t]     (the "ear")
 *   z_p        += dt * ( z_p*(alpha_p + i*omega_p) + k_p*F_p(t) )
 *   omega_p    += -dt * eps_p * k_p * F_p(t) * sin(arg z_p)
 *   omega_p     = clamp(omega_p, omega0_p/sqrt(2), omega0_p*sqrt(2))
 *
 * alpha_p < 0 (damping); omega0_p is the unit's SEEDED register (see
 * twGrooveDefaultEnsemble below) -- the clamp keeps a unit's adaptive
 * frequency tracking within one octave-ish band of what it was seeded at,
 * per design section 3.2's "omega clamped to +-sqrt(2) of the unit's seed
 * register".
 */

/**
 * One ensemble unit's structural spec -- proposal 40 section 3.2's "two
 * frequency characteristics that must never be conflated": periodInTatums is
 * the TIMELINE resonance (omega0 = 2*pi / (periodInTatums * tatumPeriodSec)),
 * receptiveShape is the SPECTRAL receptive field (which regions drive it).
 * This is exactly the structure a trained mode freezes (twGrooveTrainStructure
 * below) while omega itself keeps adapting per material.
 */
enum class twGrooveReceptiveShape {
    Broad,       // uniform weight across every region (the neutral gauge)
    LowHeavy,    // weight falls off with region index (Hove 2014: low-band
                 // timing dominance; "bounce")
    HighHeavy,   // weight rises with region index ("limbs")
};

struct twGroovePendulumUnitSpec {
    std::string            name;
    double                 periodInTatums = 1.0;
    twGrooveReceptiveShape  receptive      = twGrooveReceptiveShape::Broad;

    // A BAR is not reliably a fixed multiple of the recovered tatum -- found
    // on the M0 fixture set: d_twobar.wav's own finest regular pulse is the
    // quarter note (4 tatums/bar in the usual 8-tatums/bar eighth-note
    // reading), so a "2-bar" unit seeded as tatum*16 misses the true period
    // by 2x, outside the omega clamp's capture range. A unit with
    // periodInBars > 0 is seeded from a SEPARATE, slower autocorrelation
    // search (twGrooveRecoverPeriodByAutocorrelation over a lag range wide
    // enough to contain a bar however many tatums it turns out to have) for
    // its own bar period, times periodInBars -- never from periodInTatums,
    // which is then ignored. 0.0 (the default) means "tatum-relative", used
    // for the fast units (reference/bounce/limbs) where a bar-scale search
    // buys nothing and costs an extra autocorrelation pass.
    double                 periodInBars   = 0.0;

    // Dynamics. k = drive coupling, eps = frequency-adaptation rate,
    // dampingCycles = the number of periods the FREE (undriven) unit takes to
    // decay to 1/e -- alpha_p is derived from it as
    // -omega0_p / (2*pi*dampingCycles), so every unit's damping is expressed
    // in the SAME dimensionless "how many of my own cycles do I remember"
    // unit rather than as a raw rate that would mean something different at
    // every timescale.
    double k             = 1.5;
    double eps           = 0.05;
    double dampingCycles = 4.0;
};

/**
 * The default 5-unit ensemble, proposal 40 section 3.2's seeding table
 * (reference / bounce / limbs / sway / 2-bar). The two FAST units (bounce,
 * limbs) are tatum-relative (periodInTatums), which assumes a 4/4 bar of 8
 * tatums -- reasonable for them because a beat/half-bar mis-seed by up to 2x
 * still lands inside the omega clamp's capture range on ordinary material.
 * The two SLOW units (sway, "2-bar" = twobar) are bar-relative
 * (periodInBars, see that field's doc) precisely because a bar is NOT
 * reliably 8 tatums -- d_twobar.wav's own finest pulse is a quarter note (a
 * 4-tatum bar), and an 8-tatum assumption there misses the true 2-bar period
 * by 2x, outside the clamp. The REFERENCE unit is what twGroovePendulumAnalyze
 * scores every event's residual against (section 3.3: "never against the
 * pendulums the event's own bands drive") -- it is deliberately Broad (a
 * neutral gauge, not biased toward either a low or a high band).
 */
std::vector<twGroovePendulumUnitSpec> twGrooveDefaultEnsemble();

struct twGroovePendulumParams {
    std::vector<twGroovePendulumUnitSpec> ensemble = twGrooveDefaultEnsemble();

    // Tatum-seeding search range, handed to twGrooveBaselineAnalyze's tatum
    // recovery (proposal 40 section 3.2: "seed the tatum/beat/bar rates from
    // the baseline's tatum estimate -- that is legitimate seeding, not
    // cheating"). Kept here (not hardcoded) so a caller varies it alongside
    // the ensemble.
    double minTatumSec = 0.1;
    double maxTatumSec = 2.0;

    // Bar-scale seed for any unit with periodInBars > 0 (see that field's
    // doc): an ABSOLUTE anchor, not a multiple of the recovered tatum.
    // Tried a second, slower autocorrelation search first (a multiple of the
    // TATUM) and measured it genuinely ambiguous: a bar is 8 tatums when the
    // material's finest pulse is an eighth note and 4 tatums when it is a
    // quarter note (both are real content in the M0 fixture set -- compare
    // d_twobar.wav's quarter-note pulse against every other fixture's
    // eighth), and a SINGLE tatum-multiple search band cannot distinguish
    // the two without knowing which it is ahead of time -- exactly the
    // octave/meter-ambiguity problem the beat-tracking literature treats as
    // genuinely hard, not a bug to code around in an M0 spike. An ABSOLUTE
    // anchor sidesteps it: proposal 40 section 3.2 already names one --
    // "the ~2 Hz body-resonance curve (van Noorden & Moelants) as the
    // ensemble's overall tempo weighting" -- 2 Hz beat is a 2 s bar at 4/4,
    // 120 BPM, close enough to every M0 fixture's own bar (1.97-2.4 s over
    // the fixture set's 100-122 BPM range) to sit inside the omega clamp's
    // +-sqrt(2) capture band from EVERY one of them, so the adaptive
    // frequency law (which stays on regardless of this seed -- design
    // section 3.2, "training freezes the STRUCTURE, never the clock") still
    // finds the material's own true bar rate.
    double defaultBarPeriodSec = 2.0;

    // Confidence readout (design section 3.3/AC (h)): the reference unit's
    // |z|^2, normalized by the run's OWN peak |z_ref|^2, must fall below
    // confidenceFloor during a fill/break and climb back above it within
    // confidenceRelockSec of coherent drive resuming.
    double confidenceFloor = 0.35;

    twGrooveStatsParams stats;
};

/** One ensemble unit's full per-hop trajectory (pass 1's stored state). */
struct twGroovePendulumUnitTrajectory {
    std::string         name;
    std::vector<double> phaseWrapped;     // arg(z), radians, (-pi,pi]
    std::vector<double> omega;            // rad/sec
    std::vector<double> magnitude;        // |z|
    std::vector<double> driveF;           // F_p(t)
    double              omega0 = 0.0;     // the seed register (rad/sec)
};

/** Proposal 40 section 3.5: the pass-2-only counter-tension readout. */
struct twGrooveCounterTension {
    std::string name;
    double      meanSinDeltaPhi = 0.0;   // the "lean" (static counter-tension)
    double      varSinDeltaPhi  = 0.0;   // the "jitter cost" (corrective tension)
    double      meanF           = 0.0;   // the drive factor, reported SEPARATELY
                                          // (section 3.5: a loudness confound)
};

struct twGroovePendulumResult {
    double                                       tatumPeriodSec = 0.0;
    twGrooveResidualReport                       residuals;          // pass 2, vs frozen reference
    std::vector<double>                          confidence;         // per hop, [0,1]
    std::vector<twGroovePendulumUnitTrajectory>   unitTrajectories;   // pass 1, one per unit
    std::vector<twGrooveCounterTension>           counterTension;     // one per unit
    std::vector<double>                           unitMeanR;          // one per unit: time-mean |z|^2
    uint32_t                                      hopFrames = 0;
    uint32_t                                      rate      = 0;
};

/**
 * Free-running (adaptive) mode: seeds omega from THIS field's own baseline
 * tatum estimate (twGrooveBaselineAnalyze over the same field), then runs the
 * two-pass ensemble (pass 1: adaptive phase/frequency tracking over the whole
 * material, storing every unit's trajectory; pass 2: scores every front-end
 * event against the FROZEN reference-unit phase trajectory from pass 1).
 * Returns a default-constructed (tatumPeriodSec == 0) result if the field has
 * no recoverable tatum (mirrors twGrooveBaselineAnalyze's honest-empty rule).
 */
twGroovePendulumResult twGroovePendulumAnalyze( const twGrooveField &field,
                                                const twGroovePendulumParams &params );

/**
 * Structure-only trained mode (design section 3.2: "two modes, one switch --
 * training freezes the STRUCTURE, never the clock"). What crosses from
 * training to scoring is the ensemble SPEC (params.ensemble -- receptive
 * fields and period RATIOS, already tempo-relative) plus the mu(region)
 * pattern recovered during training; omega itself is ALWAYS re-seeded from
 * the material actually being analyzed (never frozen), so a tempo difference
 * between training and scoring material cannot be misread as a static lean.
 */
struct twGrooveTrainedStructure {
    twGroovePendulumParams paramsUsed;                 // the ensemble spec that was frozen
    std::vector<bool>      trainedHasRegion;            // size nRegions
    std::vector<double>    trainedMuMsByRegion;          // size nRegions
};

/** Trains (freezes) structure from one field -- runs the normal two-pass
 * analysis on trainField and keeps its ensemble spec plus its recovered
 * mu(region) pattern. */
twGrooveTrainedStructure twGroovePendulumTrainStructure( const twGrooveField &trainField,
                                                          const twGroovePendulumParams &params );

/**
 * Scores scoreField using structure.paramsUsed's ensemble spec (the frozen
 * receptive fields/period ratios); omega is re-seeded from scoreField's OWN
 * baseline tatum, so this is otherwise identical to twGroovePendulumAnalyze.
 * The trained mu(region) pattern is NOT applied here -- it is exposed on
 * `structure` for the CALLER to compare against the returned result's
 * residuals (the "phantom mu shift" a caller measures is
 * result.residuals.perRegion[r].muMs - structure.trainedMuMsByRegion[r]).
 */
twGroovePendulumResult twGroovePendulumScoreWithStructure( const twGrooveField &scoreField,
                                                            const twGrooveTrainedStructure &structure );

#endif
