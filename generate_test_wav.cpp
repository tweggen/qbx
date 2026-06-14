#include <sndfile.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>

int main() {
    // File path
    const char *filename = "test_sawtooth.wav";

    // WAV parameters
    const int SAMPLE_RATE = 48000;
    const int CHANNELS = 1;
    const int BITS_PER_SAMPLE = 16;
    const int VALUES_PER_RAMP = 65536;  // -32768 to +32767
    const int SAMPLES_PER_VALUE = 4;
    const int NUM_RAMPS = 4;

    // Calculate total samples
    long total_samples = VALUES_PER_RAMP * SAMPLES_PER_VALUE * NUM_RAMPS;

    printf("Generating test WAV file: %s\n", filename);
    printf("  Sample rate: %d Hz\n", SAMPLE_RATE);
    printf("  Channels: %d\n", CHANNELS);
    printf("  Bit depth: %d\n", BITS_PER_SAMPLE);
    printf("  Total samples: %ld\n", total_samples);
    printf("  Duration: %.2f seconds\n", (float)total_samples / SAMPLE_RATE);

    // Create and configure SF_INFO
    SF_INFO sfInfo;
    sfInfo.samplerate = SAMPLE_RATE;
    sfInfo.channels = CHANNELS;
    sfInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    // Open file for writing
    SNDFILE *outfile = sf_open(filename, SFM_WRITE, &sfInfo);
    if (!outfile) {
        printf("Error: Could not open %s for writing\n", filename);
        printf("  %s\n", sf_strerror(nullptr));
        return 1;
    }

    printf("File opened successfully\n");

    // Generate and write sawtooth data
    std::vector<float> buffer(VALUES_PER_RAMP * SAMPLES_PER_VALUE);

    for (int ramp = 0; ramp < NUM_RAMPS; ++ramp) {
        printf("Generating ramp %d/%d...\n", ramp + 1, NUM_RAMPS);

        // Generate sawtooth: from -32768 to +32767
        // Values go from -1.0 to ~0.9999... when normalized
        int idx = 0;
        for (int value = -32768; value <= 32767; ++value) {
            // Normalize to [-1, 1] range
            float normalized = (float)value / 32768.0f;

            // Each value held for SAMPLES_PER_VALUE samples
            for (int sample = 0; sample < SAMPLES_PER_VALUE; ++sample) {
                buffer[idx++] = normalized;
            }
        }

        // Write buffer to file
        sf_count_t num_written = sf_writef_float(outfile, buffer.data(), buffer.size());
        if (num_written != buffer.size()) {
            printf("Error: Only wrote %ld/%zu frames\n", num_written, buffer.size());
            sf_close(outfile);
            return 1;
        }
    }

    printf("Data written successfully\n");

    // Close file
    if (sf_close(outfile) != 0) {
        printf("Error: Could not close file\n");
        return 1;
    }

    printf("File closed successfully\n");
    printf("Test WAV file created: %s\n", filename);

    return 0;
}
