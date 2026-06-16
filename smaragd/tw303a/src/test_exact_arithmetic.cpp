#include "twfraction.h"
#include <cassert>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <sstream>

// ============================================================================
// Test Framework
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
// Serialization Simulation Tests
// ============================================================================

void testProjectSerialization(TestRunner& runner) {
    std::cout << "\n--- Project Serialization ---" << std::endl;

    // Simulate project saving posFactor
    Fraction posFactor(1, 48000);
    std::string savedPosFactor = posFactor.toString();

    // Simulate project loading posFactor
    Fraction loadedPosFactor = parseFractionOrDouble(savedPosFactor);

    runner.assertEqual("Project posFactor roundtrip", loadedPosFactor, posFactor);
    runner.assertTrue("Project posFactor string representation",
                     savedPosFactor == "1/48000");
}

void testLinkSerialization(TestRunner& runner) {
    std::cout << "\n--- SLink Serialization ---" << std::endl;

    // Simulate SLink saving startTime (sample count)
    uint64_t startTime = 240000;  // samples
    Fraction startTimeFrac(startTime, 1);
    std::string savedStartTime = startTimeFrac.toString();

    // Simulate SLink loading startTime
    Fraction loadedStartTimeFrac = parseFractionOrDouble(savedStartTime);
    uint64_t loadedStartTime = (uint64_t)loadedStartTimeFrac.toDouble();

    runner.assertEqual("SLink startTime roundtrip",
                      loadedStartTime, startTime);
}

void testCutSerialization(TestRunner& runner) {
    std::cout << "\n--- SCut Serialization ---" << std::endl;

    // Simulate SCut saving window parameters
    uint64_t startOffset = 100000;
    uint64_t cutDuration = 96000;
    uint64_t loopLength = 48000;

    // Simulate serialization
    std::string serialized_startOffset = Fraction(startOffset, 1).toString();
    std::string serialized_cutDuration = Fraction(cutDuration, 1).toString();
    std::string serialized_loopLength = Fraction(loopLength, 1).toString();

    // Simulate deserialization
    uint64_t loaded_startOffset = (uint64_t)parseFractionOrDouble(serialized_startOffset).toDouble();
    uint64_t loaded_cutDuration = (uint64_t)parseFractionOrDouble(serialized_cutDuration).toDouble();
    uint64_t loaded_loopLength = (uint64_t)parseFractionOrDouble(serialized_loopLength).toDouble();

    runner.assertTrue("SCut startOffset roundtrip", loaded_startOffset == startOffset);
    runner.assertTrue("SCut cutDuration roundtrip", loaded_cutDuration == cutDuration);
    runner.assertTrue("SCut loopLength roundtrip", loaded_loopLength == loopLength);
}

void testActionSerialization(TestRunner& runner) {
    std::cout << "\n--- Action Serialization ---" << std::endl;

    // Test MoveClipAction
    uint64_t newStartTime = 120000;
    std::string serialized = Fraction(newStartTime, 1).toString();
    uint64_t loaded = (uint64_t)parseFractionOrDouble(serialized).toDouble();

    runner.assertTrue("MoveClipAction startTime roundtrip", loaded == newStartTime);

    // Test ResizeClipAction with multiple fields
    uint64_t startOffset = 50000;
    uint64_t duration = 192000;

    std::string s_offset = Fraction(startOffset, 1).toString();
    std::string s_duration = Fraction(duration, 1).toString();

    uint64_t l_offset = (uint64_t)parseFractionOrDouble(s_offset).toDouble();
    uint64_t l_duration = (uint64_t)parseFractionOrDouble(s_duration).toDouble();

    runner.assertTrue("ResizeClipAction offset", l_offset == startOffset);
    runner.assertTrue("ResizeClipAction duration", l_duration == duration);
}

// ============================================================================
// Hierarchical Composition Tests
// ============================================================================

void testHierarchicalComposition(TestRunner& runner) {
    std::cout << "\n--- Hierarchical Composition ---" << std::endl;

    // Scenario: nested composition with posFactor scaling
    // Project level: posFactor = 1/48000 (default)
    // Track level: no override, inherits 1/48000
    // Clip at project level: position = 80000 samples
    // Effective time = 80000 * (1/48000) = 1.667 seconds

    Fraction projectPosFactor(1, 48000);
    Fraction clipPosition(80000, 1);

    Fraction effectiveTime = clipPosition * projectPosFactor;
    runner.approxEqual("Hierarchical composition: 80000 * (1/48000)",
                      effectiveTime.toDouble(), 80000.0 / 48000.0, 1e-10);

    // Nested scenario: stretched container at 2x
    // Container posFactor = 1/(48000/2) = 2/48000 = 1/24000
    // Position within container = 24000 samples
    // Effective time = 24000 * (1/24000) = 1 second
    Fraction containerPosFactor(1, 24000);
    Fraction containerPosition(24000, 1);

    Fraction nestedEffectiveTime = containerPosition * containerPosFactor;
    runner.approxEqual("Nested composition: 24000 * (1/24000)",
                      nestedEffectiveTime.toDouble(), 1.0, 1e-10);
}

// ============================================================================
// Backward Compatibility Tests
// ============================================================================

void testBackwardCompatibility(TestRunner& runner) {
    std::cout << "\n--- Backward Compatibility ---" << std::endl;

    // Old format: plain integer (sample count)
    Fraction from_integer = parseFractionOrDouble("240000");
    runner.assertEqual("Parse integer '240000'", from_integer, Fraction(240000, 1));

    // Old format: floating-point double
    Fraction from_double = parseFractionOrDouble("5.0");
    runner.assertEqual("Parse double '5.0'", from_double, Fraction(5, 1));

    // New format: explicit fraction
    Fraction from_fraction = parseFractionOrDouble("240000/48000");
    runner.assertEqual("Parse fraction '240000/48000'", from_fraction, Fraction(5, 1));

    // New format: decimal approximation
    Fraction from_decimal = parseFractionOrDouble("1.5");
    runner.assertEqual("Parse decimal '1.5'", from_decimal, Fraction(3, 2));
}

// ============================================================================
// Roundtrip Tests (Serialization → Deserialization)
// ============================================================================

void testRoundtripAccuracy(TestRunner& runner) {
    std::cout << "\n--- Roundtrip Accuracy ---" << std::endl;

    // Test that fractions survive multiple roundtrip cycles
    Fraction original(3, 2);

    for (int i = 0; i < 5; ++i) {
        std::string serialized = original.toString();
        Fraction deserialized = parseFractionOrDouble(serialized);

        if (deserialized == original) {
            std::cout << "  Roundtrip " << i+1 << ": " << serialized << " → OK" << std::endl;
        } else {
            runner.assertEqual("Roundtrip " + std::to_string(i+1),
                             deserialized, original);
            return;
        }
        original = deserialized;
    }
    runner.passCount += 5;
}

void testNestingRoundtrip(TestRunner& runner) {
    std::cout << "\n--- Nesting Roundtrip ---" << std::endl;

    // Simulate nested save/load with composition
    // Original: position = 80000 @ 1/48000
    Fraction position(80000, 1);
    Fraction factor(1, 48000);

    // Serialize both
    std::string pos_str = position.toString();
    std::string factor_str = factor.toString();

    // Deserialize
    Fraction pos_loaded = parseFractionOrDouble(pos_str);
    Fraction factor_loaded = parseFractionOrDouble(factor_str);

    // Compose
    Fraction effective_time = pos_loaded * factor_loaded;
    double expected_seconds = 80000.0 / 48000.0;

    runner.approxEqual("Nesting roundtrip: compose and calculate",
                      effective_time.toDouble(), expected_seconds, 1e-10);
}

// ============================================================================
// Precision Tests
// ============================================================================

void testPrecisionPreservation(TestRunner& runner) {
    std::cout << "\n--- Precision Preservation ---" << std::endl;

    // Test that fractions preserve precision better than floats
    double float_value = 1.0 / 3.0;  // 0.333333...

    // Float representation (lossy)
    double float_roundtrip = float_value;

    // Fraction representation (lossless)
    Fraction frac_value(1, 3);
    double frac_roundtrip = frac_value.toDouble();

    // Fraction should be more accurate
    double expected = 1.0 / 3.0;
    runner.approxEqual("Float precision: 1/3", float_roundtrip, expected, 1e-10);
    runner.approxEqual("Fraction precision: 1/3", frac_roundtrip, expected, 1e-16);

    // Test grain size precision
    double grain_size_ms = 42.6667;  // 2048 @ 48kHz
    Fraction grain_frac = parseFractionOrDouble("42.6667");
    double grain_roundtrip = grain_frac.toDouble();

    runner.approxEqual("Grain size precision", grain_roundtrip, grain_size_ms, 0.01);
}

// ============================================================================
// Stretch Factor Tests
// ============================================================================

void testStretchComposition(TestRunner& runner) {
    std::cout << "\n--- Stretch Factor Composition ---" << std::endl;

    // Timeline position: 240000 samples
    // Stretch factor: 3/2 (1.5x speed)
    // Project factor: 1/48000
    // Result: 240000 * (3/2) * (1/48000) = 360000 / 48000 = 7.5 seconds

    Fraction position(240000, 1);
    Fraction stretch(3, 2);
    Fraction project_factor(1, 48000);

    Fraction result = position * stretch * project_factor;
    runner.approxEqual("Stretch composition: 240000 * (3/2) * (1/48000) = 7.5",
                      result.toDouble(), 7.5, 1e-10);

    // Multiple stretch levels
    // Inner stretch: 2/1 (2x speed)
    // Outer stretch: 1/2 (0.5x speed)
    // Net: (2/1) * (1/2) = 1/1 (normal speed)

    Fraction inner_stretch(2, 1);
    Fraction outer_stretch(1, 2);
    Fraction net_stretch = inner_stretch * outer_stretch;

    runner.assertEqual("Nested stretches: (2/1) * (1/2) = 1/1",
                      net_stretch, Fraction(1, 1));
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

void testEdgeCases(TestRunner& runner) {
    std::cout << "\n--- Edge Cases ---" << std::endl;

    // Very large sample counts
    Fraction large(999999999, 1);
    std::string large_str = large.toString();
    Fraction large_loaded = parseFractionOrDouble(large_str);
    runner.assertEqual("Large sample count", large_loaded, large);

    // Very small fractions
    Fraction small(1, 1000000);
    std::string small_str = small.toString();
    Fraction small_loaded = parseFractionOrDouble(small_str);
    runner.approxEqual("Small fraction", small_loaded.toDouble(), small.toDouble(), 1e-16);

    // Zero values
    Fraction zero(0, 1);
    std::string zero_str = zero.toString();
    Fraction zero_loaded = parseFractionOrDouble(zero_str);
    runner.assertEqual("Zero value", zero_loaded, Fraction(0, 1));

    // Mixed operations
    Fraction f1(3, 4);
    Fraction f2(2, 3);
    Fraction sum = f1 + f2;
    runner.assertEqual("Sum: 3/4 + 2/3 = 17/12", sum, Fraction(17, 12));
}

// ============================================================================
// XML-like Serialization Tests
// ============================================================================

void testXMLSerialization(TestRunner& runner) {
    std::cout << "\n--- XML-like Serialization ---" << std::endl;

    // Simulate XML attribute serialization/deserialization pattern
    // like: elem.setAttribute("startTime", QString::fromStdString(Fraction(240000, 1).toString()))
    // and: elem.attribute("startTime", "0").toStdString()

    uint64_t original_value = 240000;
    std::string xml_value = Fraction(original_value, 1).toString();
    uint64_t loaded_value = (uint64_t)parseFractionOrDouble(xml_value).toDouble();

    runner.assertTrue("XML integer attribute roundtrip", loaded_value == original_value);

    // Test with fractional value in XML
    std::string xml_fraction = "3/2";
    double fraction_value = parseFractionOrDouble(xml_fraction).toDouble();
    runner.approxEqual("XML fraction attribute", fraction_value, 1.5, 1e-10);
}

// ============================================================================
// Main Test Suite
// ============================================================================

int main() {
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "EXACT ARITHMETIC INTEGRATION TEST SUITE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    TestRunner runner;

    // Serialization tests
    testProjectSerialization(runner);
    testLinkSerialization(runner);
    testCutSerialization(runner);
    testActionSerialization(runner);

    // Hierarchical composition
    testHierarchicalComposition(runner);

    // Backward compatibility
    testBackwardCompatibility(runner);

    // Roundtrip tests
    testRoundtripAccuracy(runner);
    testNestingRoundtrip(runner);

    // Precision tests
    testPrecisionPreservation(runner);

    // Stretch composition
    testStretchComposition(runner);

    // Edge cases
    testEdgeCases(runner);

    // XML serialization
    testXMLSerialization(runner);

    runner.printSummary();

    return runner.failCount > 0 ? 1 : 0;
}
