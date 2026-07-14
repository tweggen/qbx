#include "tw/core/twfraction.h"
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
// Proposal 18 Phase 0: signedness, overflow, rounding, exact-parse properties
// ============================================================================

void testSignedArithmetic(TestRunner& runner) {
    std::cout << "\n--- Signed Arithmetic (Phase 0) ---" << std::endl;

    Fraction a(3, 4), b(5, 2);

    // Subtraction is exact below zero (old implementation clamped to 0/1)
    Fraction d = a - b;
    runner.assertEqual("3/4 - 5/2 = -7/4", d, Fraction(-7, 4));
    runner.assertEqual("(a-b)+b == a", d + b, a);

    // Sign normalization: denominator always positive
    Fraction n(3, -4);
    runner.assertEqual("3/-4 normalizes to -3/4", n, Fraction(-3, 4));
    runner.assertTrue("denominator kept positive", n.denominator > 0);

    // Unary minus, abs
    runner.assertEqual("-(−3/4) == 3/4", -Fraction(-3, 4), Fraction(3, 4));
    runner.assertEqual("abs(-3/4) == 3/4", Fraction(-3, 4).abs(), Fraction(3, 4));

    // Negative multiplication/division
    runner.assertEqual("(-3/4)*(2/3) = -1/2",
                      Fraction(-3, 4) * Fraction(2, 3), Fraction(-1, 2));
    runner.assertEqual("(-3/4)/(-3/2) = 1/2",
                      Fraction(-3, 4) / Fraction(-3, 2), Fraction(1, 2));

    // Ordering across zero
    runner.assertTrue("-7/4 < 3/4", Fraction(-7, 4) < Fraction(3, 4));
    runner.assertTrue("-1/2 > -3/4", Fraction(-1, 2) > Fraction(-3, 4));

    // Negative parse/serialize roundtrip
    Fraction neg = parseFractionOrDouble("-7/4");
    runner.assertEqual("parse '-7/4'", neg, Fraction(-7, 4));
    runner.assertEqual("toString/parse roundtrip of -7/4",
                      parseFractionOrDouble(neg.toString()), neg);
}

void testOverflowSafety(TestRunner& runner) {
    std::cout << "\n--- Overflow Safety (Phase 0) ---" << std::endl;

    // Cross products beyond 64 bits: (2^40/3) + (2^40/5) has a*d + b*c
    // around 2^43 (fine), but (big/1) * (1/big) used to be the dangerous
    // shape: reduce-before-narrow must survive it.
    int64_t big = (int64_t)1 << 40;
    Fraction f1(big, 3), f2(big, 5);
    Fraction sum = f1 + f2;
    runner.assertEqual("2^40/3 + 2^40/5 = 8*2^40/15",
                      sum, Fraction(big * 8, 15));

    // Product whose UNREDUCED numerator/denominator exceed 64 bits but
    // whose reduced value is tiny: (2^62/3) * (3/2^62) == 1.
    int64_t huge = (int64_t)1 << 62;
    Fraction g1(huge, 3), g2(3, huge);
    runner.assertEqual("(2^62/3)*(3/2^62) == 1 (reduce before narrow)",
                      g1 * g2, Fraction(1, 1));

    // Comparison with large cross products must not wrap:
    // (2^61/1) > (2^61-1)/1
    runner.assertTrue("2^61 > 2^61-1 (no comparison wrap)",
                     Fraction(((int64_t)1 << 61), 1) >
                     Fraction(((int64_t)1 << 61) - 1, 1));

    // Exact integer/integer parse survives values a double cannot hold:
    // 2^60+1 is not representable in double; the exact parser keeps it.
    int64_t precise = ((int64_t)1 << 60) + 1;
    std::string s = std::to_string(precise) + "/48000";
    Fraction p = parseFractionOrDouble(s);
    runner.assertEqual("exact parse of (2^60+1)/48000",
                      p, Fraction(precise, 48000));
}

void testFloorCeil(TestRunner& runner) {
    std::cout << "\n--- floor/ceil Projections (Phase 0) ---" << std::endl;

    // The render-boundary rounding rule: starts floor, ends floor(start+len).
    runner.assertTrue("floor(7/2) == 3", Fraction(7, 2).floorToInt() == 3);
    runner.assertTrue("ceil(7/2) == 4", Fraction(7, 2).ceilToInt() == 4);
    runner.assertTrue("floor(-7/2) == -4 (floor division, not truncation)",
                     Fraction(-7, 2).floorToInt() == -4);
    runner.assertTrue("ceil(-7/2) == -3", Fraction(-7, 2).ceilToInt() == -3);
    runner.assertTrue("floor(6/2) == 3 (integral unchanged)",
                     Fraction(6, 2).floorToInt() == 3);
    runner.assertTrue("ceil(6/2) == 3", Fraction(6, 2).ceilToInt() == 3);

    // Tiling property (small exhaustive check): for exact start s and
    // lengths l1,l2, segments [floor(s),floor(s+l1)) and
    // [floor(s+l1),floor(s+l1+l2)) tile [floor(s),floor(s+l1+l2)) with
    // no gap or overlap by construction.
    bool tiled = true;
    for (int sn = -8; sn <= 8 && tiled; ++sn) {
        for (int l1n = 0; l1n <= 8 && tiled; ++l1n) {
            for (int l2n = 0; l2n <= 8 && tiled; ++l2n) {
                Fraction s(sn, 3), l1(l1n, 3), l2(l2n, 3);
                int64_t a = s.floorToInt();
                int64_t b = (s + l1).floorToInt();
                int64_t c = (s + l1 + l2).floorToInt();
                // segment1 = [a,b), segment2 = [b,c): contiguous & ordered
                if (!(a <= b && b <= c)) tiled = false;
            }
        }
    }
    runner.assertTrue("floor-rounding tiles adjacent exact intervals", tiled);
}

void testCompositionProperties(TestRunner& runner) {
    std::cout << "\n--- Composition Properties (Phase 0) ---" << std::endl;

    // stretch(a) ∘ stretch(b) == stretch(a*b), exactly — the proposal-18
    // repeated-stretch property, on a chain whose double equivalent drifts.
    Fraction stretches[] = {{44543, 48000}, {3, 2}, {48000, 44543},
                            {2, 3}, {7, 5}, {5, 7}};
    Fraction net(1, 1);
    for (const Fraction& st : stretches) net = net * st;
    runner.assertEqual("six-factor stretch chain cancels to exactly 1/1",
                      net, Fraction(1, 1));

    // map/inverse identity: x * s / s == x for awkward s and x
    Fraction x(157954, 1), st(44543, 48000);
    runner.assertEqual("x * s / s == x (exact inverse)",
                      x * st / st, x);

    // Associativity spot-check with mixed signs
    Fraction p(-7, 12), q(5, 8), r(9, 14);
    runner.assertEqual("(p*q)*r == p*(q*r)", (p * q) * r, p * (q * r));
    runner.assertEqual("(p+q)+r == p+(q+r)", (p + q) + r, p + (q + r));
}

void testDenominatorCap(TestRunner& runner) {
    std::cout << "\n--- Creation-Time Denominator Cap (Phase 2) ---" << std::endl;

    // Already-fitting fractions pass through unchanged
    runner.assertEqual("3/2 unchanged under cap 1000",
                      Fraction(3, 2).limitedTo(1000), Fraction(3, 2));

    // Classic convergent: pi's 355/113 from a huge exact ratio
    Fraction piish(3141592653, 1000000000);
    runner.assertEqual("pi ratio capped at 200 -> 355/113",
                      piish.limitedTo(200), Fraction(355, 113));

    // Negative values keep their sign
    runner.assertEqual("negative cap keeps sign",
                      (-piish).limitedTo(200), Fraction(-355, 113));

    // The gesture shape: a stretch ratio of two frame counts stays close
    // after capping (error < 1/maxDen^2 by CF convergent property)
    Fraction gest(4432157, 4800000);
    Fraction capped = gest.limitedTo((uint64_t)1 << 20);
    runner.assertTrue("capped gesture ratio within CF error bound",
                     (gest - capped).abs() < Fraction(1, (int64_t)1 << 20));
    runner.assertTrue("capped denominator respects bound",
                     capped.denominator <= ((int64_t)1 << 20));
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

    // Proposal 18 Phase 0
    testSignedArithmetic(runner);
    testOverflowSafety(runner);
    testFloorCeil(runner);
    testCompositionProperties(runner);
    testDenominatorCap(runner);

    runner.printSummary();

    return runner.failCount > 0 ? 1 : 0;
}
