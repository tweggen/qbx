#include "tw/core/twfraction.h"
#include <cassert>
#include <iostream>
#include <iomanip>
#include <cmath>

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
// Test Cases
// ============================================================================

void testFractionConstruction(TestRunner& runner) {
    std::cout << "\n--- Fraction Construction ---" << std::endl;

    Fraction f1;
    runner.assertEqual("Default constructor", f1, Fraction(0, 1));

    Fraction f2(5);
    runner.assertEqual("Constructor with numerator only", f2, Fraction(5, 1));

    Fraction f3(240000, 48000);
    runner.assertEqual("Constructor with numerator and denominator",
                      f3, Fraction(5, 1));  // Should simplify

    Fraction f4(0, 100);
    runner.assertTrue("Zero is simplified", f4.denominator == 1);
}

void testFractionSimplification(TestRunner& runner) {
    std::cout << "\n--- Fraction Simplification ---" << std::endl;

    Fraction f1(240000, 48000);
    runner.assertEqual("240000/48000 simplifies to 5/1", f1, Fraction(5, 1));

    Fraction f2(2048, 48000);
    runner.assertEqual("2048/48000 simplifies", f2,
                      Fraction(128, 3000));  // gcd = 16

    Fraction f3(1, 1);
    runner.assertEqual("1/1 stays 1/1", f3, Fraction(1, 1));

    Fraction f4(0, 0);
    runner.assertTrue("0/0 defaults to 0/1", f4.denominator == 1);
}

void testArithmeticAddition(TestRunner& runner) {
    std::cout << "\n--- Arithmetic: Addition ---" << std::endl;

    Fraction f1(1, 2);  // 0.5
    Fraction f2(1, 4);  // 0.25
    Fraction sum = f1 + f2;
    runner.assertEqual("1/2 + 1/4 = 3/4", sum, Fraction(3, 4));

    Fraction f3(2, 1);  // 2
    Fraction f4(3, 1);  // 3
    Fraction sum2 = f3 + f4;
    runner.assertEqual("2 + 3 = 5", sum2, Fraction(5, 1));
}

void testArithmeticMultiplication(TestRunner& runner) {
    std::cout << "\n--- Arithmetic: Multiplication ---" << std::endl;

    Fraction f1(3, 2);  // 1.5x
    Fraction f2(4, 3);  // 1.333x
    Fraction product = f1 * f2;
    runner.assertEqual("(3/2) * (4/3) = 2/1", product, Fraction(2, 1));

    Fraction f3(1, 2);
    Fraction f4(1, 2);
    Fraction product2 = f3 * f4;
    runner.assertEqual("(1/2) * (1/2) = 1/4", product2, Fraction(1, 4));
}

void testArithmeticDivision(TestRunner& runner) {
    std::cout << "\n--- Arithmetic: Division ---" << std::endl;

    Fraction f1(3, 2);  // 1.5
    Fraction f2(1, 2);  // 0.5
    Fraction quotient = f1 / f2;
    runner.assertEqual("(3/2) / (1/2) = 3/1", quotient, Fraction(3, 1));

    Fraction f3(1, 4);
    Fraction f4(1, 2);
    Fraction quotient2 = f3 / f4;
    runner.assertEqual("(1/4) / (1/2) = 1/2", quotient2, Fraction(1, 2));
}

void testComparison(TestRunner& runner) {
    std::cout << "\n--- Comparison Operators ---" << std::endl;

    Fraction f1(1, 2);
    Fraction f2(2, 4);  // Also 1/2
    Fraction f3(1, 4);

    runner.assertTrue("1/2 == 2/4", f1 == f2);
    runner.assertTrue("1/2 > 1/4", f1 > f3);
    runner.assertTrue("1/4 < 1/2", f3 < f1);
    runner.assertTrue("1/2 != 1/4", f1 != f3);
}

void testConversionToDouble(TestRunner& runner) {
    std::cout << "\n--- Conversion to Double ---" << std::endl;

    Fraction f1(3, 2);
    runner.approxEqual("3/2 converts to 1.5", f1.toDouble(), 1.5);

    Fraction f2(1, 3);
    runner.approxEqual("1/3 converts to 0.333...", f2.toDouble(),
                      0.333333, 1e-5);

    Fraction f3(240000, 48000);
    runner.approxEqual("240000/48000 converts to 5.0", f3.toDouble(), 5.0);
}

void testToString(TestRunner& runner) {
    std::cout << "\n--- String Representation ---" << std::endl;

    Fraction f1(5, 1);
    runner.assertTrue("5/1 formats as '5'", f1.toString() == "5");

    Fraction f2(3, 2);
    runner.assertTrue("3/2 formats as '3/2'", f2.toString() == "3/2");

    Fraction f3(240000, 48000);
    runner.assertTrue("240000/48000 simplifies to '5'",
                     f3.toString() == "5");
}

void testParserExplicitFractions(TestRunner& runner) {
    std::cout << "\n--- Parser: Explicit Fractions ---" << std::endl;

    Fraction f1 = parseFractionOrDouble("3/2");
    runner.assertEqual("Parse '3/2'", f1, Fraction(3, 2));

    Fraction f2 = parseFractionOrDouble("240000/48000");
    runner.assertEqual("Parse '240000/48000'", f2, Fraction(5, 1));

    Fraction f3 = parseFractionOrDouble("1/4");
    runner.assertEqual("Parse '1/4'", f3, Fraction(1, 4));
}

void testParserBareIntegers(TestRunner& runner) {
    std::cout << "\n--- Parser: Bare Integers ---" << std::endl;

    Fraction f1 = parseFractionOrDouble("240000");
    runner.assertEqual("Parse '240000'", f1, Fraction(240000, 1));

    Fraction f2 = parseFractionOrDouble("5");
    runner.assertEqual("Parse '5'", f2, Fraction(5, 1));

    Fraction f3 = parseFractionOrDouble("0");
    runner.assertEqual("Parse '0'", f3, Fraction(0, 1));
}

void testParserDecimals(TestRunner& runner) {
    std::cout << "\n--- Parser: Decimal Numbers ---" << std::endl;

    Fraction f1 = parseFractionOrDouble("5.0");
    runner.assertEqual("Parse '5.0'", f1, Fraction(5, 1));

    Fraction f2 = parseFractionOrDouble("1.5");
    runner.assertEqual("Parse '1.5' (known)", f2, Fraction(3, 2));

    Fraction f3 = parseFractionOrDouble("0.5");
    runner.assertEqual("Parse '0.5' (known)", f3, Fraction(1, 2));
}

void testParserKnownGrainSizes(TestRunner& runner) {
    std::cout << "\n--- Parser: Known Grain Sizes ---" << std::endl;

    // Note: grain sizes in milliseconds are approximated by continued fractions
    // 42.6667 ms ≈ 2048 samples / 48000 Hz, but as a fraction it becomes 2048/48000 = 0.0426667
    // The lookup table stores the exact fraction, which has that value
    Fraction f1 = parseFractionOrDouble("42.6667");
    // Continued fractions will approximate, and the test should verify the approximation is close
    runner.approxEqual("Parse '42.6667' approximates",
                      f1.toDouble(), 42.6667, 0.1);  // Allow 0.1 tolerance

    Fraction f2 = parseFractionOrDouble("10.6667");
    runner.approxEqual("Parse '10.6667' approximates",
                      f2.toDouble(), 10.6667, 0.1);
}

void testParserApproximations(TestRunner& runner) {
    std::cout << "\n--- Parser: Approximations (Continued Fractions) ---"
              << std::endl;

    Fraction f1 = parseFractionOrDouble("0.208333");
    // Should approximate 10000/48000 or similar
    runner.approxEqual("Parse '0.208333' approximates", f1.toDouble(),
                      0.208333, 1e-4);

    Fraction f2 = parseFractionOrDouble("0.75");
    runner.assertEqual("Parse '0.75'", f2, Fraction(3, 4));

    Fraction f3 = parseFractionOrDouble("2.0");
    runner.assertEqual("Parse '2.0'", f3, Fraction(2, 1));
}

void testNestingAndComposition(TestRunner& runner) {
    std::cout << "\n--- Nesting and Composition ---" << std::endl;

    // Scenario: position = 80000, track stretch = 3/2, project factor = 1/48000
    // Result: 80000 * (3/2) * (1/48000) = (80000 * 3/2) / 48000 = 120000 / 48000 = 2.5 seconds

    Fraction position(80000, 1);
    Fraction stretch(3, 2);
    Fraction projectFactor(1, 48000);

    Fraction result = position * stretch * projectFactor;
    runner.approxEqual("80000 × (3/2) × (1/48000) = 2.5 seconds",
                      result.toDouble(), 2.5, 1e-10);
}

void testEdgeCases(TestRunner& runner) {
    std::cout << "\n--- Edge Cases ---" << std::endl;

    // Division by zero
    Fraction f1 = parseFractionOrDouble("1/0");
    runner.assertEqual("Parse '1/0' defaults to 0/1", f1, Fraction(0, 1));

    // Empty string
    Fraction f2 = parseFractionOrDouble("");
    runner.assertEqual("Parse empty string defaults to 0/1", f2,
                      Fraction(0, 1));

    // Whitespace
    Fraction f3 = parseFractionOrDouble("  3/2  ");
    runner.assertEqual("Parse '  3/2  ' (with whitespace)", f3,
                      Fraction(3, 2));

    // Negative results are EXACT (proposal 18 Phase 0: the old clamp to
    // zero silently corrupted position deltas)
    Fraction f4(5, 2);
    Fraction f5(3, 4);
    Fraction diff = f5 - f4;  // 0.75 - 2.5 = -7/4, exact
    runner.assertEqual("Subtraction below zero is exact (-7/4)",
                      diff, Fraction(-7, 4));
    runner.assertTrue("Negative fraction detected", diff.isNegative());
    runner.assertEqual("(a-b)+b == a", diff + f4, f5);
}

// ============================================================================
// Main Test Suite
// ============================================================================

int main() {
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "FRACTION CLASS AND PARSER TEST SUITE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    TestRunner runner;

    testFractionConstruction(runner);
    testFractionSimplification(runner);
    testArithmeticAddition(runner);
    testArithmeticMultiplication(runner);
    testArithmeticDivision(runner);
    testComparison(runner);
    testConversionToDouble(runner);
    testToString(runner);
    testParserExplicitFractions(runner);
    testParserBareIntegers(runner);
    testParserDecimals(runner);
    testParserKnownGrainSizes(runner);
    testParserApproximations(runner);
    testNestingAndComposition(runner);
    testEdgeCases(runner);

    runner.printSummary();

    return runner.failCount > 0 ? 1 : 0;
}
