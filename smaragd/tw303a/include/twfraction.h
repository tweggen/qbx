#ifndef _TWFRACTION_H_
#define _TWFRACTION_H_

#include <cstdint>
#include <string>
#include <cmath>

/**
 * Exact rational number (fraction) for lossless time-related arithmetic.
 * Stores numerator and denominator; all operations maintain exact precision.
 */
struct Fraction {
    uint64_t numerator;
    uint64_t denominator;

    // Constructors
    Fraction() : numerator(0), denominator(1) {}
    Fraction(uint64_t num, uint64_t den = 1)
        : numerator(num), denominator(den) {
        if (denominator == 0) denominator = 1;
        simplify();
    }

    // Arithmetic operators (maintain exact precision)
    Fraction operator+(const Fraction& other) const {
        // a/b + c/d = (a*d + b*c)/(b*d)
        uint64_t num = numerator * other.denominator +
                       denominator * other.numerator;
        uint64_t den = denominator * other.denominator;
        return {num, den};
    }

    Fraction operator-(const Fraction& other) const {
        // a/b - c/d = (a*d - b*c)/(b*d)
        if (numerator * other.denominator <
            denominator * other.numerator) {
            return {0, 1};  // Clamp to zero if negative
        }
        uint64_t num = numerator * other.denominator -
                       denominator * other.numerator;
        uint64_t den = denominator * other.denominator;
        return {num, den};
    }

    Fraction operator*(const Fraction& other) const {
        // (a/b) * (c/d) = (a*c)/(b*d)
        uint64_t num = numerator * other.numerator;
        uint64_t den = denominator * other.denominator;
        return {num, den};
    }

    Fraction operator/(const Fraction& other) const {
        // (a/b) / (c/d) = (a*d)/(b*c)
        if (other.numerator == 0) return {0, 1};  // Error
        uint64_t num = numerator * other.denominator;
        uint64_t den = denominator * other.numerator;
        return {num, den};
    }

    // Comparison operators
    bool operator==(const Fraction& other) const {
        // Compare by cross-multiplication to avoid floating point
        return numerator * other.denominator ==
               denominator * other.numerator;
    }

    bool operator!=(const Fraction& other) const {
        return !(*this == other);
    }

    bool operator<(const Fraction& other) const {
        return numerator * other.denominator <
               denominator * other.numerator;
    }

    bool operator<=(const Fraction& other) const {
        return *this < other || *this == other;
    }

    bool operator>(const Fraction& other) const {
        return numerator * other.denominator >
               denominator * other.numerator;
    }

    bool operator>=(const Fraction& other) const {
        return *this > other || *this == other;
    }

    // Simplify by dividing by GCD
    void simplify() {
        if (numerator == 0) {
            denominator = 1;
            return;
        }
        uint64_t g = gcd(numerator, denominator);
        if (g > 1) {
            numerator /= g;
            denominator /= g;
        }
    }

    // Convert to double (for display/debugging)
    double toDouble() const {
        if (denominator == 0) return 0.0;
        return (double)numerator / (double)denominator;
    }

    // Convert to string "numerator/denominator"
    std::string toString() const {
        if (denominator == 1) {
            return std::to_string(numerator);
        }
        return std::to_string(numerator) + "/" + std::to_string(denominator);
    }

    // GCD using Euclidean algorithm
    static uint64_t gcd(uint64_t a, uint64_t b) {
        while (b != 0) {
            uint64_t temp = b;
            b = a % b;
            a = temp;
        }
        return a;
    }

    // Check if this is an integer (denominator == 1)
    bool isInteger() const {
        return denominator == 1;
    }

    // Check if this is zero
    bool isZero() const {
        return numerator == 0;
    }

    // Check if this is approximately equal within tolerance (for testing)
    bool approximatelyEqual(const Fraction& other, double tolerance = 1e-9) const {
        double diff = std::fabs(toDouble() - other.toDouble());
        return diff < tolerance;
    }
};

// ============================================================================
// Parsing Functions
// ============================================================================

/**
 * Parse a fraction or double from string.
 * Handles formats: "240000", "240000/48000", "5.0", "1.5", "42.6667"
 *
 * Strategy:
 * 1. Explicit fraction: "numerator/denominator" → parse both parts
 * 2. Known decimal: "42.6667" → lookup table → exact fraction
 * 3. Unknown decimal: "0.208333" → continued fractions → approximation
 * 4. Integer: "240000" → 240000/1
 */
Fraction parseFractionOrDouble(const std::string& str);

/**
 * Convert a decimal number to a fraction using continued fractions algorithm.
 * Produces increasingly accurate rational approximations.
 *
 * @param value The decimal value to convert
 * @param maxDenominator Maximum denominator to allow (prevents huge fractions)
 * @return Best rational approximation within the denominator limit
 */
Fraction doubleToFraction(double value, uint64_t maxDenominator = 1000000);

/**
 * Convert a decimal to a fraction, checking known values first.
 * Uses lookup table for common grain sizes and stretch factors.
 * Falls back to continued fractions for unknown values.
 *
 * @param value The decimal value to convert
 * @return Exact fraction if known, best approximation otherwise
 */
Fraction doubleToFractionWithLookup(double value);

#endif // _TWFRACTION_H_
