#include "tw/analysis/audio_analysis.h"
#include <sndfile.h>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace audio {

AcousticMetrics analyzeWavFile(const std::string &filename, std::string &error)
{
    SF_INFO sfInfo;
    std::memset(&sfInfo, 0, sizeof(sfInfo));

    SNDFILE *infile = sf_open(filename.c_str(), SFM_READ, &sfInfo);
    if (!infile) {
        error = std::string("Failed to open WAV file: ") + sf_strerror(nullptr);
        return AcousticMetrics{0, 0, 0, 0, 0, 0, 0};
    }

    // Analyze entire file
    AcousticMetrics metrics = analyzeWavFileRegion(filename, 0, sfInfo.frames, -1, error);
    sf_close(infile);
    return metrics;
}

AcousticMetrics analyzeWavFileRegion(const std::string &filename,
                                     int64_t startFrame, int64_t frameCount,
                                     int channelIndex, std::string &error)
{
    SF_INFO sfInfo;
    std::memset(&sfInfo, 0, sizeof(sfInfo));

    SNDFILE *infile = sf_open(filename.c_str(), SFM_READ, &sfInfo);
    if (!infile) {
        error = std::string("Failed to open WAV file: ") + sf_strerror(nullptr);
        return AcousticMetrics{0, 0, 0, 0, 0, 0, 0};
    }

    // Validate region
    if (startFrame < 0 || startFrame >= sfInfo.frames) {
        error = "Start frame out of bounds";
        sf_close(infile);
        return AcousticMetrics{0, 0, 0, 0, 0, 0, 0};
    }

    int64_t endFrame = std::min(startFrame + frameCount, sfInfo.frames);
    int64_t framesToRead = endFrame - startFrame;

    if (framesToRead <= 0) {
        error = "Invalid frame range";
        sf_close(infile);
        return AcousticMetrics{0, 0, 0, 0, 0, 0, 0};
    }

    // Seek to start position
    sf_seek(infile, startFrame, SEEK_SET);

    // Read audio data
    const int BUFFER_SIZE = 4096;
    std::vector<float> buffer(BUFFER_SIZE * sfInfo.channels);

    double sumSquares = 0.0;
    double peakAmp = 0.0;
    double minAmp = 0.0;
    double maxAmp = 0.0;
    int64_t samplesRead = 0;

    while (samplesRead < framesToRead) {
        int64_t toRead = std::min((int64_t)BUFFER_SIZE, framesToRead - samplesRead);
        sf_count_t nRead = sf_readf_float(infile, buffer.data(), toRead);

        if (nRead <= 0) break;

        // Process samples
        for (sf_count_t i = 0; i < nRead; ++i) {
            for (int ch = 0; ch < sfInfo.channels; ++ch) {
                float sample = buffer[i * sfInfo.channels + ch];

                // Skip if analyzing specific channel
                if (channelIndex >= 0 && ch != channelIndex) continue;

                // RMS energy
                sumSquares += sample * sample;

                // Amplitude bounds
                double absSample = std::abs((double)sample);
                peakAmp = (absSample > peakAmp) ? absSample : peakAmp;
                double dblSample = (double)sample;
                minAmp = (dblSample < minAmp) ? dblSample : minAmp;
                maxAmp = (dblSample > maxAmp) ? dblSample : maxAmp;
            }
        }

        samplesRead += nRead;
    }

    sf_close(infile);

    // Compute RMS
    int64_t channelsToCount = (channelIndex >= 0) ? 1 : sfInfo.channels;
    int64_t totalSamples = samplesRead * channelsToCount;
    double rmsEnergy = (totalSamples > 0) ? std::sqrt(sumSquares / totalSamples) : 0.0;

    return AcousticMetrics{
        rmsEnergy,
        peakAmp,
        minAmp,
        maxAmp,
        samplesRead,
        sfInfo.channels,
        sfInfo.samplerate
    };
}

bool isEnergyInRange(const AcousticMetrics &metrics, double minRms, double maxRms)
{
    return metrics.rmsEnergy >= minRms && metrics.rmsEnergy <= maxRms;
}

bool isPeakInRange(const AcousticMetrics &metrics, double maxPeak)
{
    return metrics.peakAmplitude <= maxPeak;
}

double estimateFundamental(const std::string &filename,
                           int64_t startFrame, int64_t frameCount,
                           int channelIndex, std::string &error)
{
    SF_INFO sfInfo;
    std::memset(&sfInfo, 0, sizeof(sfInfo));

    SNDFILE *infile = sf_open(filename.c_str(), SFM_READ, &sfInfo);
    if (!infile) {
        error = std::string("Failed to open WAV file: ") + sf_strerror(nullptr);
        return 0.0;
    }
    if (startFrame < 0 || startFrame >= sfInfo.frames) {
        error = "Start frame out of bounds";
        sf_close(infile);
        return 0.0;
    }

    int64_t endFrame = (frameCount < 0)
                     ? sfInfo.frames
                     : std::min(startFrame + frameCount, (int64_t) sfInfo.frames);
    int64_t framesToRead = endFrame - startFrame;
    if (framesToRead <= 0) {
        error = "Invalid frame range";
        sf_close(infile);
        return 0.0;
    }

    // Read the region into one mono track (mixed, or the requested channel).
    sf_seek(infile, startFrame, SEEK_SET);
    const int BUFFER_SIZE = 4096;
    std::vector<float> buffer(BUFFER_SIZE * sfInfo.channels);
    std::vector<double> mono;
    mono.reserve((size_t) framesToRead);

    int64_t framesRead = 0;
    while (framesRead < framesToRead) {
        int64_t toRead = std::min((int64_t) BUFFER_SIZE, framesToRead - framesRead);
        sf_count_t nRead = sf_readf_float(infile, buffer.data(), toRead);
        if (nRead <= 0) break;
        for (sf_count_t i = 0; i < nRead; ++i) {
            double v = 0.0;
            if (channelIndex >= 0 && channelIndex < sfInfo.channels) {
                v = buffer[i * sfInfo.channels + channelIndex];
            } else {
                for (int ch = 0; ch < sfInfo.channels; ++ch)
                    v += buffer[i * sfInfo.channels + ch];
                v /= sfInfo.channels;
            }
            mono.push_back(v);
        }
        framesRead += nRead;
    }
    sf_close(infile);

    if ((int64_t) mono.size() < 4) {
        error = "Region too short for pitch detection";
        return 0.0;
    }

    // Remove the mean: a DC offset biases every lag equally and can swamp the
    // periodic structure of an asymmetric waveform like a sawtooth.
    double mean = 0.0;
    for (double v : mono) mean += v;
    mean /= (double) mono.size();
    for (double &v : mono) v -= mean;

    // Lag search range: 40 Hz .. 4 kHz, clipped to what the region can support
    // (a lag beyond half the region gives too few overlapping samples to be
    // meaningful).
    const double rate = (double) sfInfo.samplerate;
    int64_t minLag = (int64_t) (rate / 4000.0);
    int64_t maxLag = (int64_t) (rate / 40.0);
    if (minLag < 2) minLag = 2;
    if (maxLag > (int64_t) mono.size() / 2) maxLag = (int64_t) mono.size() / 2;
    if (maxLag <= minLag) {
        error = "Region too short for the requested frequency range";
        return 0.0;
    }

    double energy = 0.0;
    for (double v : mono) energy += v * v;
    if (energy <= 1e-12) {
        error = "Region is silent";
        return 0.0;
    }

    // Plain autocorrelation, normalised by the zero-lag energy. The first
    // strong peak after the initial decay is the period.
    std::vector<double> corr((size_t) (maxLag + 1), 0.0);
    for (int64_t lag = minLag; lag <= maxLag; ++lag) {
        double sum = 0.0;
        int64_t n = (int64_t) mono.size() - lag;
        for (int64_t i = 0; i < n; ++i) sum += mono[(size_t) i] * mono[(size_t) (i + lag)];
        corr[(size_t) lag] = sum / (double) n;
    }

    double globalMax = 0.0;
    int64_t globalLag = 0;
    for (int64_t lag = minLag; lag <= maxLag; ++lag) {
        if (corr[(size_t) lag] > globalMax) { globalMax = corr[(size_t) lag]; globalLag = lag; }
    }
    if (globalLag <= 0 || globalMax <= 0.0) {
        error = "No periodicity found";
        return 0.0;
    }

    // Take the FIRST strong peak, not the tallest one. Autocorrelation peaks at
    // every MULTIPLE of the period, and for a sawtooth the 2x lag routinely
    // edges out the true one — which reports a transposed clip at its original
    // pitch (the classic octave error; measured exactly that on the +1200 cent
    // render). The first local maximum within 15% of the global one is the
    // period itself.
    int64_t bestLag = globalLag;
    for (int64_t lag = minLag + 1; lag < maxLag; ++lag) {
        if (corr[(size_t) lag] >= 0.85 * globalMax
            && corr[(size_t) lag] >= corr[(size_t) (lag - 1)]
            && corr[(size_t) lag] >= corr[(size_t) (lag + 1)]) {
            bestLag = lag;
            break;
        }
    }

    // Parabolic interpolation around the peak: the true period rarely lands on
    // an integer sample, and a whole-sample lag quantises badly at high pitches
    // (at 880 Hz one sample of lag is already ~1.8%).
    double refined = (double) bestLag;
    if (bestLag > minLag && bestLag < maxLag) {
        double y0 = corr[(size_t) (bestLag - 1)];
        double y1 = corr[(size_t) bestLag];
        double y2 = corr[(size_t) (bestLag + 1)];
        double denom = y0 - 2.0 * y1 + y2;
        if (std::abs(denom) > 1e-12) refined += 0.5 * (y0 - y2) / denom;
    }
    if (refined <= 0.0) {
        error = "No periodicity found";
        return 0.0;
    }
    return rate / refined;
}

}  // namespace audio
