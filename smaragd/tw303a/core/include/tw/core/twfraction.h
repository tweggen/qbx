#ifndef _TWFRACTION_H_
#define _TWFRACTION_H_

#include <cstdint>
#include <cassert>
#include <string>
#include <cmath>

/**
 * Exact rational number (fraction) for lossless time-related arithmetic
 * (EXACT_ARITHMETIC_DESIGN.md; hardened by proposal 18 Phase 0).
 *
 * Invariants:
 *   - denominator > 0 always (sign lives in the numerator)
 *   - stored reduced (gcd(|num|, den) == 1)
 *
 * All arithmetic and comparisons go through 128-bit intermediates, so any
 * operation whose REDUCED result fits int64 is exact — cross products like
 * a*d + b*c cannot silently wrap. If even the reduced result overflows
 * int64 the value saturates and a debug assert fires (this is the
 * denominator red-line of proposal 18; creation-time clamping is supposed
 * to keep real projects far away from it).
 *
 * Subtraction is EXACT and may be negative — the pre-proposal-18 clamp to
 * zero is gone; position deltas depend on this.
 */
struct Fraction {
    int64_t numerator;
    int64_t denominator;   // > 0 always

    Fraction() : numerator(0), denominator(1) {}
    // A zero denominator degrades gracefully to den=1 (parse boundary /
    // legacy file leniency); internal arithmetic asserts instead.
    Fraction(int64_t num, int64_t den = 1)
        : numerator(num), denominator(den) {
        if (denominator == 0) denominator = 1;
        normalize();
    }

#if !defined(__SIZEOF_INT128__)
#error "Fraction requires __int128 (64-bit GCC/Clang) for overflow-safe arithmetic"
#endif
    typedef __int128 wide_t;

    // Arithmetic (exact; reduced result must fit int64, else saturate+assert)
    Fraction operator+(const Fraction& other) const {
        wide_t num = (wide_t)numerator * other.denominator +
                     (wide_t)other.numerator * denominator;
        wide_t den = (wide_t)denominator * other.denominator;
        return fromWide(num, den);
    }

    Fraction operator-(const Fraction& other) const {
        wide_t num = (wide_t)numerator * other.denominator -
                     (wide_t)other.numerator * denominator;
        wide_t den = (wide_t)denominator * other.denominator;
        return fromWide(num, den);
    }

    Fraction operator*(const Fraction& other) const {
        wide_t num = (wide_t)numerator * other.numerator;
        wide_t den = (wide_t)denominator * other.denominator;
        return fromWide(num, den);
    }

    Fraction operator/(const Fraction& other) const {
        if (other.numerator == 0) {
            assert(!"Fraction: division by zero");
            return {0, 1};
        }
        wide_t num = (wide_t)numerator * other.denominator;
        wide_t den = (wide_t)denominator * other.numerator;
        return fromWide(num, den);
    }

    Fraction operator-() const { return Fraction(-numerator, denominator); }

    // Comparisons: exact via 128-bit cross products (denominators > 0, so
    // the inequality direction is preserved).
    bool operator==(const Fraction& other) const {
        return (wide_t)numerator * other.denominator ==
               (wide_t)other.numerator * denominator;
    }
    bool operator!=(const Fraction& other) const { return !(*this == other); }
    bool operator<(const Fraction& other) const {
        return (wide_t)numerator * other.denominator <
               (wide_t)other.numerator * denominator;
    }
    bool operator<=(const Fraction& other) const { return !(other < *this); }
    bool operator>(const Fraction& other) const { return other < *this; }
    bool operator>=(const Fraction& other) const { return !(*this < other); }

    // Reduce and normalize sign (denominator > 0).
    void normalize() {
        if (denominator < 0) { numerator = -numerator; denominator = -denominator; }
        if (numerator == 0) { denominator = 1; return; }
        uint64_t a = (numerator < 0) ? (uint64_t)(-(numerator + 1)) + 1
                                     : (uint64_t)numerator;
        uint64_t g = gcd(a, (uint64_t)denominator);
        if (g > 1) {
            numerator /= (int64_t)g;
            denominator /= (int64_t)g;
        }
    }
    // Back-compat name.
    void simplify() { normalize(); }

    // APPROXIMATE conversion — display, debugging, and non-time math
    // (amplitude ramps, UI pixels) only. Never feed the result back into
    // position arithmetic; that is what the exact ops are for.
    double approxDouble() const {
        return (double)numerator / (double)denominator;
    }
    // Back-compat alias for approxDouble(); prefer the explicit name.
    double toDouble() const { return approxDouble(); }

    // Exact integer projections (floor/ceil division, correct for negatives).
    // floorToInt is THE rounding of proposal 18's render boundary: interval
    // starts floor; exclusive ends are floor(start + len) computed exactly.
    int64_t floorToInt() const {
        int64_t q = numerator / denominator;
        int64_t r = numerator % denominator;
        return (r < 0) ? q - 1 : q;
    }
    int64_t ceilToInt() const {
        int64_t q = numerator / denominator;
        int64_t r = numerator % denominator;
        return (r > 0) ? q + 1 : q;
    }

    std::string toString() const {
        if (denominator == 1) {
            return std::to_string(numerator);
        }
        return std::to_string(numerator) + "/" + std::to_string(denominator);
    }

    static uint64_t gcd(uint64_t a, uint64_t b) {
        while (b != 0) {
            uint64_t temp = b;
            b = a % b;
            a = temp;
        }
        return a;
    }

    bool isInteger() const { return denominator == 1; }
    bool isZero() const { return numerator == 0; }
    bool isNegative() const { return numerator < 0; }

    Fraction abs() const {
        return (numerator < 0) ? Fraction(-numerator, denominator) : *this;
    }

    // Approximate comparison (testing/diagnostics only).
    bool approximatelyEqual(const Fraction& other, double tolerance = 1e-9) const {
        double diff = std::fabs(approxDouble() - other.approxDouble());
        return diff < tolerance;
    }

private:
    // Build a reduced Fraction from 128-bit numerator/denominator.
    // Reduces FIRST (so any representable value survives), then narrows;
    // saturates with a debug assert if the reduced value still cannot fit.
    static Fraction fromWide(wide_t num, wide_t den) {
        if (den == 0) { assert(!"Fraction: zero denominator"); return {0, 1}; }
        if (den < 0) { num = -num; den = -den; }
        if (num == 0) return {0, 1};

        unsigned __int128 a = (num < 0) ? (unsigned __int128)(-num)
                                        : (unsigned __int128)num;
        unsigned __int128 b = (unsigned __int128)den;
        // 128-bit gcd (Euclid)
        while (b != 0) {
            unsigned __int128 t = b;
            b = a % b;
            a = t;
        }
        num /= (wide_t)a;
        den /= (wide_t)a;

        const wide_t I64MAX = (wide_t)INT64_MAX;
        const wide_t I64MIN = (wide_t)INT64_MIN;
        if (num > I64MAX || num < I64MIN || den > I64MAX) {
            assert(!"Fraction: reduced value overflows int64 (denominator red-line)");
            // Saturate: keep the sign and an extreme magnitude rather than wrap.
            Fraction f;
            f.numerator = (num < 0) ? INT64_MIN : INT64_MAX;
            f.denominator = 1;
            return f;
        }
        Fraction f;
        f.numerator = (int64_t)num;
        f.denominator = (int64_t)den;
        return f;
    }
};

// ============================================================================
// Parsing Functions
// ============================================================================

/**
 * Parse a fraction or double from string.
 * Handles formats: "240000", "-240000", "240000/48000", "5.0", "1.5", "-3/2"
 *
 * Strategy:
 * 1. Explicit INTEGER fraction "n/d": parsed EXACTLY (no double round-trip)
 * 2. Fraction with decimal parts "3.5/2.0": ratio via continued fractions
 * 3. Known decimal: lookup table → exact fraction
 * 4. Unknown decimal: continued fractions approximation
 * 5. Bare integer: value/1
 */
Fraction parseFractionOrDouble(const std::string& str);

/**
 * Convert a decimal to a fraction using the continued fractions algorithm.
 * Handles negative values. maxDenominator bounds the approximation.
 */
Fraction doubleToFraction(double value, uint64_t maxDenominator = 1000000);

/**
 * Convert a decimal to a fraction, checking known values first
 * (common stretch factors / grain sizes), falling back to continued
 * fractions.
 */
Fraction doubleToFractionWithLookup(double value);

#endif // _TWFRACTION_H_
