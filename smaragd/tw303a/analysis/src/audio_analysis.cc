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

}  // namespace audio
