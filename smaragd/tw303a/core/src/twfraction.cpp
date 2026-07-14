#include "tw/core/twfraction.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

// ============================================================================
// Known Decimal Values Lookup Table
// ============================================================================

struct KnownDecimal {
    double value;
    int64_t numerator;
    int64_t denominator;
};

static const KnownDecimal KNOWN_DECIMALS[] = {
    // Stretch factors / tempo ratios
    {0.5,     1,     2},      // 50% speed
    {0.75,    3,     4},      // 75% speed
    {1.0,     1,     1},      // Normal speed
    {1.5,     3,     2},      // 150% speed (1.5x)
    {2.0,     2,     1},      // 200% speed (2x)
    {2.5,     5,     2},      // 250% speed (2.5x)

    // Grain sizes @ 48kHz (seconds)
    {0.0426667, 2048,  48000},  // 2048 samples / 48000 = 42.6667 ms
    {0.0106667, 512,   48000},  // 512 samples / 48000 = 10.6667 ms
    {0.0053333, 256,   48000},  // 256 samples / 48000

    // Duration values
    {0.208333, 10000, 48000}, // Default duration (10000 samples @ 48kHz)

    // Sentinel
    {-1, 0, 0}
};

// ============================================================================
// Exact denominator cap (integer continued fractions on the fraction itself)
// ============================================================================

Fraction Fraction::limitedTo(uint64_t maxDen) const {
    if (maxDen < 1) maxDen = 1;
    if ((uint64_t)denominator <= maxDen) return *this;

    bool negative = numerator < 0;
    uint64_t n = negative ? (uint64_t)(-(numerator + 1)) + 1 : (uint64_t)numerator;
    uint64_t d = (uint64_t)denominator;

    // Continued-fraction convergents p/q of n/d, integer arithmetic only.
    uint64_t p0 = 1, q0 = 0;        // convergent k-2
    uint64_t p1 = n / d, q1 = 1;    // convergent k-1 (integer part)
    uint64_t r  = n % d;

    while (r != 0) {
        n = d; d = r;
        uint64_t a = n / d;
        r = n % d;
        uint64_t p2 = a * p1 + p0;
        uint64_t q2 = a * q1 + q0;
        if (q2 > maxDen) break;
        p0 = p1; q0 = q1;
        p1 = p2; q1 = q2;
    }

    Fraction f((int64_t)p1, (int64_t)q1);
    return negative ? -f : f;
}

// ============================================================================
// Continued Fractions Algorithm
// ============================================================================

/**
 * Convert a decimal to a fraction using the continued fractions algorithm.
 * Produces the best rational approximation within maxDenominator.
 * Sign is peeled off first; the core runs on the magnitude.
 */
Fraction doubleToFraction(double value, uint64_t maxDenominator) {
    if (std::isnan(value) || std::isinf(value)) {
        return {0, 1};
    }

    bool negative = value < 0.0;
    double v = negative ? -value : value;

    // Effectively an integer?
    if (std::fabs(v - std::round(v)) < 1e-10) {
        int64_t whole = (int64_t)std::round(v);
        return Fraction(negative ? -whole : whole, 1);
    }

    int64_t wholePart = (int64_t)std::floor(v);
    double remainder = v - wholePart;

    uint64_t num_prev = 1, den_prev = 0;
    uint64_t num_curr = (uint64_t)wholePart, den_curr = 1;

    const int MAX_ITERATIONS = 20;
    const double EPSILON = 1e-10;

    for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
        if (remainder < EPSILON) {
            break;
        }
        if (den_curr > maxDenominator) {
            break;
        }

        remainder = 1.0 / remainder;
        int64_t a = (int64_t)std::floor(remainder);
        remainder -= a;

        uint64_t num_next = (uint64_t)a * num_curr + num_prev;
        uint64_t den_next = (uint64_t)a * den_curr + den_prev;

        if (den_next > maxDenominator || num_next > (uint64_t)1e15 ||
            den_next > (uint64_t)1e15) {
            break;  // Return current best approximation
        }

        num_prev = num_curr;
        den_prev = den_curr;
        num_curr = num_next;
        den_curr = den_next;
    }

    Fraction f((int64_t)num_curr, (int64_t)den_curr);
    return negative ? -f : f;
}

// ============================================================================
// Parser with Lookup Table
// ============================================================================

Fraction doubleToFractionWithLookup(double value) {
    const double TOLERANCE = 1e-6;

    bool negative = value < 0.0;
    double v = negative ? -value : value;

    for (int i = 0; KNOWN_DECIMALS[i].value >= 0; ++i) {
        double diff = std::fabs(v - KNOWN_DECIMALS[i].value);
        if (diff < TOLERANCE) {
            Fraction f(KNOWN_DECIMALS[i].numerator,
                       KNOWN_DECIMALS[i].denominator);
            return negative ? -f : f;
        }
    }

    return doubleToFraction(value, 1000000);
}

// ============================================================================
// Main Parser Function
// ============================================================================

// True iff `s` is a plain (optionally signed) decimal integer that fits
// int64. Used for the exact "n/d" fast path.
static bool parseInt64Exact(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') i = 1;
    if (i >= s.size()) return false;
    for (size_t k = i; k < s.size(); ++k) {
        if (!std::isdigit((unsigned char)s[k])) return false;
    }
    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) return false;
        out = (int64_t)v;
        return true;
    } catch (const std::exception&) {
        return false;  // out of range
    }
}

static std::string trimmed(const std::string& in) {
    std::string s = in;
    s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
    s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
    return s;
}

Fraction parseFractionOrDouble(const std::string& str) {
    std::string s = trimmed(str);

    if (s.empty()) {
        return {0, 1};
    }

    // Case 1: Explicit fraction "numerator/denominator"
    size_t slashPos = s.find('/');
    if (slashPos != std::string::npos) {
        std::string numStr = trimmed(s.substr(0, slashPos));
        std::string denStr = trimmed(s.substr(slashPos + 1));

        // Exact fast path: integer/integer parses WITHOUT any double
        // round-trip, so large exact fractions (positions, long
        // denominator chains) survive serialization bit-exactly.
        int64_t inum, iden;
        if (parseInt64Exact(numStr, inum) && parseInt64Exact(denStr, iden)) {
            if (iden == 0) {
                std::cerr << "Fraction parser: division by zero in " << str
                          << std::endl;
                return {0, 1};
            }
            return Fraction(inum, iden);
        }

        // Legacy/lenient path: decimal parts ("3.5/2.0") via doubles.
        try {
            double num = std::stod(numStr);
            double den = std::stod(denStr);
            if (den == 0) {
                std::cerr << "Fraction parser: division by zero in " << str
                          << std::endl;
                return {0, 1};
            }
            return doubleToFraction(num / den);
        } catch (const std::exception& e) {
            std::cerr << "Fraction parser error parsing '" << str << "': "
                      << e.what() << std::endl;
            return {0, 1};
        }
    }

    // Case 2: Plain number (integer or decimal)
    int64_t ival;
    if (parseInt64Exact(s, ival)) {
        return Fraction(ival, 1);
    }
    try {
        double value = std::stod(s);
        return doubleToFractionWithLookup(value);
    } catch (const std::exception& e) {
        std::cerr << "Fraction parser: cannot parse '" << str << "': "
                  << e.what() << std::endl;
        return {0, 1};
    }
}

// ============================================================================
// String Parsing Utilities (for Qt integration, optional)
// ============================================================================

#ifdef HAVE_QT
#include <QString>

Fraction parseFractionOrDouble(const QString& qstr) {
    return parseFractionOrDouble(qstr.toStdString());
}

#endif
