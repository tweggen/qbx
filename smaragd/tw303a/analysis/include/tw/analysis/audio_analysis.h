#ifndef _AUDIO_ANALYSIS_H_
#define _AUDIO_ANALYSIS_H_

#include <string>
#include <vector>
#include <cstdint>

namespace audio {

/**
 * Acoustic properties of an audio segment.
 * All values computed from normalized float32 samples [-1.0, +1.0].
 */
struct AcousticMetrics {
    double rmsEnergy;        // Root mean square (loudness measure)
    double peakAmplitude;    // Highest absolute value
    double minAmplitude;     // Lowest signed value
    double maxAmplitude;     // Highest signed value
    int64_t frameCount;      // Total frames analyzed
    int channelCount;        // Number of channels
    int sampleRate;          // Samples per second
};

/**
 * Header-only facts about an audio file: how long it is, and in what format.
 */
struct AudioFileInfo {
    int64_t frameCount;      // Frames in the file (0 is legal and meaningful)
    int channelCount;
    int sampleRate;
};

/**
 * Read an audio file's header without decoding it.
 *
 * Deliberately separate from analyzeWavFile(): the analysers refuse an empty
 * region ("Start frame out of bounds"), which is the right answer for an RMS
 * question and the wrong one for "how long is this file" — a zero-frame render
 * (an empty arrangement) is a valid file whose length is exactly 0.
 *
 * @return false on a file that cannot be opened; `error` then says why.
 */
bool readAudioFileInfo(const std::string &filename, AudioFileInfo &info,
                       std::string &error);

/**
 * Read ONE CHANNEL of a region as normalized floats (proposal 21 L1b).
 *
 * Every other entry point here REDUCES a region to a number - an RMS, a peak,
 * a fundamental, a position code. Cross-correlating a monitored capture
 * against the file that was played into the input needs the SAMPLES, and the
 * alternative was a second libsndfile reader inside the testkit, which is how
 * two readers come to disagree about channel selection and about what
 * "frameCount < 0" means.
 *
 * `channelIndex < 0` pools every channel by AVERAGING them, matching what the
 * analysers mean by -1. `frameCount < 0` reads to the end of the file. A
 * region that starts past the end is not an error: `out` comes back empty.
 *
 * @return false only when the file cannot be opened; `error` then says why.
 */
bool readAudioRegion(const std::string &filename, int64_t startFrame,
                     int64_t frameCount, int channelIndex,
                     std::vector<float> &out, int &sampleRate,
                     std::string &error);

/**
 * Read a WAV file and analyze acoustic properties.
 *
 * `channelIndex` defaults to -1 (all channels pooled), which is what this
 * signature meant before it had the parameter at all. It exists because the
 * whole-file path used to HARD-CODE -1: a caller that had a channel in hand
 * silently lost it. A whole-file analysis is just a
 * region analysis with frameCount < 0, so this only forwards.
 *
 * @param filename Path to WAV file
 * @param error Output string for error messages
 * @param channelIndex Which channel to analyze (-1 for all channels pooled)
 * @return Acoustic metrics, or zero-filled metrics on error
 */
AcousticMetrics analyzeWavFile(const std::string &filename, std::string &error,
                               int channelIndex = -1);

/**
 * Analyze a specific region of a WAV file.
 *
 * A `channelIndex` at or beyond the file's channel count is an ERROR, not an
 * empty selection: skipping every channel would otherwise report RMS 0 / peak
 * 0, which reads exactly like "the render is silent" and would let a typo pass
 * for a real finding.
 *
 * @param filename Path to WAV file
 * @param startFrame Starting frame index (0-based)
 * @param frameCount Number of frames to analyze (< 0 = to the end of the file)
 * @param channelIndex Which channel to analyze (-1 for all channels pooled)
 * @param error Output string for error messages
 * @return Acoustic metrics for the region
 */
AcousticMetrics analyzeWavFileRegion(const std::string &filename,
                                     int64_t startFrame, int64_t frameCount,
                                     int channelIndex, std::string &error);

/**
 * Per-channel RMS of a region, plus the RMS of the DIFFERENCE between two
 * channels.
 *
 * The discriminator behind `assert-channels-differ`. Two
 * separate questions, because they fail differently:
 *
 *  - `rmsA` vs `rmsB` — do the channels carry different LEVELS? This is what
 *    catches a sink that duplicates one bus into every channel.
 *  - `rmsDiff` — is the sample-by-sample difference non-zero? Two channels can
 *    hold the same level and completely different audio (opposite polarity, a
 *    different partial, a different clip), and a level comparison calls those
 *    identical.
 *
 * One pass, so both are always available to the caller's message even when only
 * one of them is being asserted on.
 *
 * @param filename Path to WAV file
 * @param startFrame Starting frame index (0-based)
 * @param frameCount Number of frames to analyze (< 0 = to the end of the file)
 * @param channelA First channel index
 * @param channelB Second channel index
 * @param rmsA Out: RMS of channel A over the region
 * @param rmsB Out: RMS of channel B over the region
 * @param rmsDiff Out: RMS of (A - B) over the region
 * @param error Output string for error messages
 * @return True when the region was read and the three values are meaningful
 */
bool compareWavChannels(const std::string &filename,
                        int64_t startFrame, int64_t frameCount,
                        int channelA, int channelB,
                        double &rmsA, double &rmsB, double &rmsDiff,
                        std::string &error);

/**
 * Check if RMS energy falls within expected range.
 * Useful for verifying audio wasn't silenced or clipped.
 *
 * @param metrics Acoustic metrics to check
 * @param minRms Minimum acceptable RMS (e.g., 0.01 for "not silent")
 * @param maxRms Maximum acceptable RMS (e.g., 0.99 for "not clipped")
 * @return True if within bounds
 */
bool isEnergyInRange(const AcousticMetrics &metrics, double minRms, double maxRms);

/**
 * Check if peak amplitude is within acceptable range.
 *
 * @param metrics Acoustic metrics to check
 * @param maxPeak Maximum peak amplitude (e.g., 0.95 to avoid clipping)
 * @return True if peak <= maxPeak
 */
bool isPeakInRange(const AcousticMetrics &metrics, double maxPeak);

/**
 * Estimate the fundamental frequency (Hz) of a region by autocorrelation.
 *
 * The pitch gate for the grain transposition tests: RMS/peak cannot tell a
 * transposed render from an untransposed one, but the detected f0 can. Works
 * on a strongly periodic signal (the sawtooth fixture); it is not a
 * general-purpose polyphonic pitch tracker.
 *
 * The region is mixed down (or a single channel taken), mean-removed, and
 * correlated over lags corresponding to 40 Hz .. 4 kHz; the winning lag is
 * refined by parabolic interpolation.
 *
 * @param filename     Path to WAV file
 * @param startFrame   Starting frame index (0-based)
 * @param frameCount   Number of frames to analyze (-1 = whole file)
 * @param channelIndex Channel to analyze (-1 = all channels mixed)
 * @param error        Output string for error messages
 * @return Fundamental frequency in Hz, or 0.0 on error / no periodicity found
 */
double estimateFundamental(const std::string &filename,
                           int64_t startFrame, int64_t frameCount,
                           int channelIndex, std::string &error);

/**
 * Result of decoding a position-coded window (tw/core/position_code.h).
 */
struct PositionDecode {
    int64_t sourceFrame = -1;    ///< first frame of the decoded block, -1 = none
    int     blockIndex  = -1;    ///< decoded block index, -1 = none
    double  confidence  = 0.0;   ///< winning bin magnitude / runner-up's
    bool    silent      = false; ///< window was below the silence floor
};

/**
 * Decode WHERE a window of audio came from, using the integer-cycle tone
 * staircase encoding (tw/core/position_code.h).
 *
 * The position gate. RMS, peak and f0 are all position-BLIND: a render that
 * plays the right material from the wrong place passes every one of them. Fed
 * audio that originates from the position-coded fixture (tests/test_position.wav,
 * written by gen_position_fixture), this reports the SOURCE frame the window
 * carries — so "the clip played, but 4096 frames late" becomes a failure with a
 * number in it.
 *
 * `frameCount` should be kBlockFrames (4096); the encoding is designed so that
 * any window of that length lying INSIDE one block decodes exactly, at any
 * phase offset. A window straddling a block boundary lights two bins and comes
 * back with a low `confidence` rather than an arbitrary pick of the two.
 *
 * Reading past the end of the file is not an error: the window is analysed
 * short, and a window entirely past the end reports silent=true. That is what
 * makes "nothing plays after the fixture ends" assertable.
 *
 * @param filename     Path to WAV file
 * @param startFrame   Starting frame index (0-based)
 * @param frameCount   Window length in frames (4096 recommended)
 * @param channelIndex Channel to decode (-1 = all channels mixed)
 * @param error        Output string for error messages
 * @return Decode result; sourceFrame == -1 when nothing was decoded
 */
PositionDecode decodePositionAt(const std::string &filename,
                                int64_t startFrame, int64_t frameCount,
                                int channelIndex, std::string &error);

}  // namespace audio

#endif
