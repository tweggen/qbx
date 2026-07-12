#include "tw/core/twfraction.h"
#include <cassert>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <vector>
#include <sstream>

// ============================================================================
// Serialization Roundtrip Test Suite
// ============================================================================

class TestRunner {
public:
    int passCount = 0;
    int failCount = 0;

    void assertTrue(const std::string& testName, bool condition) {
        if (condition) {
            passCount++;
            std::cout << "✓ " << testName << std::endl;
        } else {
            failCount++;
            std::cout << "✗ " << testName << std::endl;
        }
    }

    void assertEqual(const std::string& testName, const Fraction& actual,
                    const Fraction& expected) {
        if (actual == expected) {
            passCount++;
            std::cout << "✓ " << testName << std::endl;
        } else {
            failCount++;
            std::cout << "✗ " << testName << " (got " << actual.toString()
                      << ", expected " << expected.toString() << ")"
                      << std::endl;
        }
    }

    void approxEqual(const std::string& testName, double actual,
                    double expected, double tolerance = 1e-6) {
        if (std::fabs(actual - expected) < tolerance) {
            passCount++;
            std::cout << "✓ " << testName << std::endl;
        } else {
            failCount++;
            std::cout << "✗ " << testName << " (got " << actual
                      << ", expected " << expected << ")" << std::endl;
        }
    }

    void printSummary() {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "Tests passed: " << passCount << std::endl;
        std::cout << "Tests failed: " << failCount << std::endl;
        std::cout << "Total: " << (passCount + failCount) << std::endl;
        std::cout << std::string(60, '=') << std::endl;
    }
};

// ============================================================================
// Complex Composition Scenarios
// ============================================================================

void testComplexTimelineScenarios(TestRunner& runner) {
    std::cout << "\n--- Complex Timeline Scenarios ---" << std::endl;

    // Scenario 1: Multiple clips at different positions
    std::vector<uint64_t> clip_positions = {0, 96000, 192000, 288000};
    Fraction project_factor(1, 48000);

    std::cout << "  Scenario 1: Four clips at different positions" << std::endl;
    for (size_t i = 0; i < clip_positions.size(); ++i) {
        uint64_t pos = clip_positions[i];
        Fraction pos_frac(pos, 1);

        // Serialize
        std::string serialized = pos_frac.toString();

        // Deserialize
        Fraction loaded = parseFractionOrDouble(serialized);

        // Verify roundtrip
        if (loaded == pos_frac) {
            std::cout << "    Clip " << i << ": position " << pos << " → OK" << std::endl;
        } else {
            runner.failCount++;
            return;
        }

        // Calculate effective time
        Fraction effective_time = loaded * project_factor;
        double expected_seconds = (double)pos / 48000.0;
        runner.approxEqual("Clip " + std::to_string(i) + " effective time",
                          effective_time.toDouble(), expected_seconds, 1e-10);
    }
}

void testNestedGroupScenarios(TestRunner& runner) {
    std::cout << "\n--- Nested Group Scenarios ---" << std::endl;

    // Scenario: Group with 2x stretch contains a clip at position 48000
    // Outer (project): factor = 1/48000
    // Middle (group with stretch 2x): effective position = position * 2
    // Inner (clip): position = 48000 samples in original content
    //
    // Timeline position in group: 48000 * 2 = 96000
    // Effective timeline position: 96000 * (1/48000) = 2 seconds

    Fraction clip_position(48000, 1);
    Fraction group_stretch(2, 1);
    Fraction project_factor(1, 48000);

    // Serialize all three values
    std::string s_clip = clip_position.toString();
    std::string s_stretch = group_stretch.toString();
    std::string s_factor = project_factor.toString();

    std::cout << "  Serialized clip position: " << s_clip << std::endl;
    std::cout << "  Serialized group stretch: " << s_stretch << std::endl;
    std::cout << "  Serialized project factor: " << s_factor << std::endl;

    // Deserialize
    Fraction l_clip = parseFractionOrDouble(s_clip);
    Fraction l_stretch = parseFractionOrDouble(s_stretch);
    Fraction l_factor = parseFractionOrDouble(s_factor);

    // Compose: position in group timeline
    Fraction group_timeline_pos = l_clip * l_stretch;
    runner.approxEqual("Group timeline position: 48000 * 2",
                      group_timeline_pos.toDouble(), 96000.0, 1e-10);

    // Compose: effective time
    Fraction effective_time = group_timeline_pos * l_factor;
    runner.approxEqual("Effective time: 96000 * (1/48000)",
                      effective_time.toDouble(), 2.0, 1e-10);
}

void testActionSequenceRoundtrip(TestRunner& runner) {
    std::cout << "\n--- Action Sequence Roundtrip ---" << std::endl;

    // Simulate a sequence of undo/redo actions
    struct ActionState {
        uint64_t startTime;
        uint64_t startOffset;
        uint64_t duration;
        double stretch;
    };

    std::vector<ActionState> actions = {
        {0, 0, 96000, 1.0},
        {96000, 10000, 96000, 1.0},
        {192000, 10000, 144000, 1.5},
        {96000, 10000, 96000, 2.0},
    };

    std::cout << "  Simulating " << actions.size() << " action roundtrips" << std::endl;

    for (size_t i = 0; i < actions.size(); ++i) {
        ActionState& action = actions[i];

        // Serialize
        std::string s_startTime = Fraction(action.startTime, 1).toString();
        std::string s_startOffset = Fraction(action.startOffset, 1).toString();
        std::string s_duration = Fraction(action.duration, 1).toString();

        // Deserialize
        uint64_t l_startTime = (uint64_t)parseFractionOrDouble(s_startTime).toDouble();
        uint64_t l_startOffset = (uint64_t)parseFractionOrDouble(s_startOffset).toDouble();
        uint64_t l_duration = (uint64_t)parseFractionOrDouble(s_duration).toDouble();

        // Verify
        bool match = (l_startTime == action.startTime) &&
                    (l_startOffset == action.startOffset) &&
                    (l_duration == action.duration);

        if (match) {
            std::cout << "    Action " << i << ": roundtrip OK" << std::endl;
            runner.passCount++;
        } else {
            std::cout << "    Action " << i << ": FAILED" << std::endl;
            runner.failCount++;
        }
    }
}

void testSampleRateCompatibility(TestRunner& runner) {
    std::cout << "\n--- Sample Rate Compatibility ---" << std::endl;

    // Same clip position, different project sample rates
    uint64_t clip_position = 240000;  // samples (always)
    int sample_rates[] = {44100, 48000, 96000};

    for (int sr : sample_rates) {
        Fraction factor(1, sr);
        Fraction position(clip_position, 1);

        // Serialize/deserialize
        std::string s_pos = position.toString();
        std::string s_factor = factor.toString();

        Fraction l_pos = parseFractionOrDouble(s_pos);
        Fraction l_factor = parseFractionOrDouble(s_factor);

        // Calculate time in seconds
        Fraction effective_time = l_pos * l_factor;
        double expected_time = 240000.0 / sr;

        runner.approxEqual("Sample rate " + std::to_string(sr) + ": 240000 samples",
                          effective_time.toDouble(), expected_time, 1e-10);
    }
}

// ============================================================================
// Granular Synthesis Parameters
// ============================================================================

void testGrainParametersRoundtrip(TestRunner& runner) {
    std::cout << "\n--- Grain Parameters Roundtrip ---" << std::endl;

    // Grain sizes at different sample rates (in samples)
    struct GrainSize {
        uint64_t samples;
        int srate;
        const char* name;
    };

    GrainSize sizes[] = {
        {2048, 48000, "2048 samples @ 48kHz = 42.67ms"},
        {512, 48000, "512 samples @ 48kHz = 10.67ms"},
        {4096, 48000, "4096 samples @ 48kHz = 85.33ms"},
        {2048, 44100, "2048 samples @ 44.1kHz = 46.44ms"},
    };

    for (const auto& grain : sizes) {
        // Store as samples
        Fraction grain_frac(grain.samples, 1);
        std::string serialized = grain_frac.toString();

        // Load back
        Fraction loaded = parseFractionOrDouble(serialized);
        uint64_t restored = (uint64_t)loaded.toDouble();

        bool match = (restored == grain.samples);
        std::cout << "  " << grain.name << ": " << (match ? "OK" : "FAILED") << std::endl;

        if (match) {
            runner.passCount++;
        } else {
            runner.failCount++;
        }
    }
}

// ============================================================================
// Time-Stretched Clip Scenarios
// ============================================================================

void testTimeStretchedClipComposition(TestRunner& runner) {
    std::cout << "\n--- Time-Stretched Clip Composition ---" << std::endl;

    // Scenario: Original clip 96000 samples (2 seconds @ 48kHz)
    // When stretched to 1.5x: effective duration = 96000 * 1.5 / 48000 = 3 seconds
    // Position on timeline: 192000 samples
    // Effective timeline position: 192000 / 48000 = 4 seconds
    // Effective clip end: 4 + 3 = 7 seconds

    Fraction clip_duration(96000, 1);
    Fraction stretch_factor(3, 2);
    Fraction project_factor(1, 48000);
    Fraction timeline_position(192000, 1);

    // Serialize
    std::string s_duration = clip_duration.toString();
    std::string s_stretch = stretch_factor.toString();
    std::string s_factor = project_factor.toString();
    std::string s_position = timeline_position.toString();

    // Deserialize
    Fraction l_duration = parseFractionOrDouble(s_duration);
    Fraction l_stretch = parseFractionOrDouble(s_stretch);
    Fraction l_factor = parseFractionOrDouble(s_factor);
    Fraction l_position = parseFractionOrDouble(s_position);

    // Calculate
    Fraction stretched_duration = l_duration * l_stretch * l_factor;
    Fraction timeline_time = l_position * l_factor;
    Fraction end_time = timeline_time + stretched_duration;

    runner.approxEqual("Time-stretched clip duration: 96000 * 1.5 / 48000",
                      stretched_duration.toDouble(), 3.0, 1e-10);
    runner.approxEqual("Timeline position: 192000 / 48000",
                      timeline_time.toDouble(), 4.0, 1e-10);
    runner.approxEqual("Clip end time: 4 + 3",
                      end_time.toDouble(), 7.0, 1e-10);
}

// ============================================================================
// Precision Stress Tests
// ============================================================================

void testPrecisionStress(TestRunner& runner) {
    std::cout << "\n--- Precision Stress Tests ---" << std::endl;

    // Test with very large and very small values
    uint64_t large_value = 999999999;
    Fraction large_frac(large_value, 1);

    // 1000 roundtrips
    Fraction current = large_frac;
    for (int i = 0; i < 100; ++i) {
        std::string serialized = current.toString();
        current = parseFractionOrDouble(serialized);
    }

    bool large_ok = (current.numerator == large_value);
    runner.assertTrue("Large value 100 roundtrips", large_ok);

    // Test with very small fractions
    Fraction small_frac(1, 1000000);
    current = small_frac;
    for (int i = 0; i < 100; ++i) {
        std::string serialized = current.toString();
        current = parseFractionOrDouble(serialized);
    }

    bool small_ok = (current == small_frac);
    runner.assertTrue("Small fraction 100 roundtrips", small_ok);
}

// ============================================================================
// Mixed Integer and Fraction Operations
// ============================================================================

void testMixedOperations(TestRunner& runner) {
    std::cout << "\n--- Mixed Integer/Fraction Operations ---" << std::endl;

    // Mix of integer and fractional values in a project
    std::vector<std::string> values = {
        "240000",       // integer (samples)
        "3/2",          // explicit fraction (stretch)
        "1/48000",      // explicit fraction (factor)
        "5.0",          // decimal (becomes 5/1)
        "1.5",          // decimal (becomes 3/2)
    };

    std::cout << "  Parsing mixed value types:" << std::endl;
    for (const auto& val : values) {
        Fraction parsed = parseFractionOrDouble(val);
        std::cout << "    '" << val << "' → " << parsed.toString();

        // Verify roundtrip
        std::string roundtrip = parsed.toString();
        Fraction reparsed = parseFractionOrDouble(roundtrip);

        if (reparsed == parsed) {
            std::cout << " (roundtrip OK)" << std::endl;
            runner.passCount++;
        } else {
            std::cout << " (roundtrip FAILED)" << std::endl;
            runner.failCount++;
        }
    }
}

// ============================================================================
// Main Test Suite
// ============================================================================

int main() {
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "SERIALIZATION ROUNDTRIP TEST SUITE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    TestRunner runner;

    // Complex scenarios
    testComplexTimelineScenarios(runner);
    testNestedGroupScenarios(runner);
    testActionSequenceRoundtrip(runner);
    testSampleRateCompatibility(runner);

    // Grain parameters
    testGrainParametersRoundtrip(runner);

    // Time-stretched clips
    testTimeStretchedClipComposition(runner);

    // Precision stress
    testPrecisionStress(runner);

    // Mixed operations
    testMixedOperations(runner);

    runner.printSummary();

    return runner.failCount > 0 ? 1 : 0;
}
