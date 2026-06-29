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
 * Read a WAV file and analyze acoustic properties.
 *
 * @param filename Path to WAV file
 * @param error Output string for error messages
 * @return Acoustic metrics, or zero-filled metrics on error
 */
AcousticMetrics analyzeWavFile(const std::string &filename, std::string &error);

/**
 * Analyze a specific region of a WAV file.
 *
 * @param filename Path to WAV file
 * @param startFrame Starting frame index (0-based)
 * @param frameCount Number of frames to analyze
 * @param channelIndex Which channel to analyze (-1 for all channels mixed)
 * @param error Output string for error messages
 * @return Acoustic metrics for the region
 */
AcousticMetrics analyzeWavFileRegion(const std::string &filename,
                                     int64_t startFrame, int64_t frameCount,
                                     int channelIndex, std::string &error);

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

}  // namespace audio

#endif
