#include "twfraction.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <iostream>

// ============================================================================
// Known Decimal Values Lookup Table
// ============================================================================

struct KnownDecimal {
    double value;
    uint64_t numerator;
    uint64_t denominator;
};

static const KnownDecimal KNOWN_DECIMALS[] = {
    // Stretch factors / tempo ratios
    {0.5,     1,     2},      // 50% speed
    {0.75,    3,     4},      // 75% speed
    {1.0,     1,     1},      // Normal speed
    {1.5,     3,     2},      // 150% speed (1.5x)
    {2.0,     2,     1},      // 200% speed (2x)
    {2.5,     5,     2},      // 250% speed (2.5x)

    // Grain sizes @ 48kHz (milliseconds)
    // Note: 2048 samples @ 48000 Hz = 42.6667 ms
    // To match "42.6667" as input, we store: 2048 * 1000 / 48000 = 42666.7 / 1000 = 42.6667
    // Or more directly: 2048000 / 48000 when input is "42.6667"
    // Actually simpler: just store as 2048, 48000 and it gives 0.0426667 seconds = 42.6667 ms
    // But we're matching decimal "42.6667" which is the millisecond value, not seconds
    // So we need: {42.6667, 2048*1000, 48000} or we match approximately
    {0.0426667, 2048,  48000},  // 2048 samples / 48000 = 0.0426667 seconds = 42.6667 ms
    {0.0106667, 512,   48000},  // 512 samples / 48000 = 0.0106667 seconds = 10.6667 ms
    {0.0053333, 256,   48000},  // 256 samples / 48000

    // Note: The grain size in milliseconds as a decimal like "42.6667" will be matched
    // by continued fractions approximation, not by exact lookup

    // Duration values
    {0.208333, 10000, 48000}, // Default duration (10000 samples @ 48kHz)
    {0.208333, 9223,  44100}, // Default duration @ 44.1kHz

    // Sentinel
    {-1, 0, 0}
};

// ============================================================================
// Continued Fractions Algorithm
// ============================================================================

/**
 * Convert a decimal to a fraction using continued fractions algorithm.
 * This produces increasingly good rational approximations.
 *
 * Algorithm:
 *   1. Extract integer part, keep remainder
 *   2. Take reciprocal of remainder
 *   3. Repeat until remainder is near zero or denominator exceeds limit
 *   4. Track convergents (best rational approximations)
 *
 * Example: 0.208333... → [0; 4, 1, 4] → 1/5
 */
Fraction doubleToFraction(double value, uint64_t maxDenominator) {
    // Handle special cases
    if (std::isnan(value) || std::isinf(value)) {
        return {0, 1};
    }

    // Check if it's effectively an integer
    if (std::fabs(value - std::round(value)) < 1e-10) {
        return {(uint64_t)std::round(value), 1};
    }

    // Initialize for continued fractions
    int64_t wholePart = (int64_t)std::floor(value);
    double remainder = value - wholePart;

    uint64_t num_prev = 1, den_prev = 0;
    uint64_t num_curr = wholePart, den_curr = 1;

    // Clamp whole part to valid range
    if (num_curr > 1e15) {
        num_curr = 1e15;
    }

    // Iterate: find convergents
    const int MAX_ITERATIONS = 20;
    const double EPSILON = 1e-10;

    for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
        // Stop if remainder is negligible
        if (remainder < EPSILON) {
            break;
        }

        // Stop if denominator would exceed limit
        if (den_curr > maxDenominator) {
            break;
        }

        // Compute next partial quotient
        remainder = 1.0 / remainder;
        int64_t a = (int64_t)std::floor(remainder);
        remainder -= a;

        // Compute next convergent
        uint64_t num_next = a * num_curr + num_prev;
        uint64_t den_next = a * den_curr + den_prev;

        // Check for overflow or exceeding limit
        if (den_next > maxDenominator || num_next > 1e15 || den_next > 1e15) {
            break;  // Return current best approximation
        }

        // Update for next iteration
        num_prev = num_curr;
        den_prev = den_curr;
        num_curr = num_next;
        den_curr = den_next;
    }

    return {num_curr, den_curr};
}

// ============================================================================
// Parser with Lookup Table
// ============================================================================

Fraction doubleToFractionWithLookup(double value) {
    // Tolerance for matching known decimals
    const double TOLERANCE = 1e-6;

    // Check known decimal values
    for (int i = 0; KNOWN_DECIMALS[i].value >= 0; ++i) {
        double diff = std::fabs(value - KNOWN_DECIMALS[i].value);
        if (diff < TOLERANCE) {
            return {KNOWN_DECIMALS[i].numerator,
                    KNOWN_DECIMALS[i].denominator};
        }
    }

    // Not in lookup table, use continued fractions
    return doubleToFraction(value, 1000000);
}

// ============================================================================
// Main Parser Function
// ============================================================================

Fraction parseFractionOrDouble(const std::string& str) {
    // Trim whitespace
    std::string s = str;
    s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
    s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);

    // Handle empty string
    if (s.empty()) {
        return {0, 1};
    }

    // Case 1: Explicit fraction "numerator/denominator"
    size_t slashPos = s.find('/');
    if (slashPos != std::string::npos) {
        try {
            std::string numStr = s.substr(0, slashPos);
            std::string denStr = s.substr(slashPos + 1);

            // Trim each part
            numStr.erase(0, numStr.find_first_not_of(" \t"));
            numStr.erase(numStr.find_last_not_of(" \t") + 1);
            denStr.erase(0, denStr.find_first_not_of(" \t"));
            denStr.erase(denStr.find_last_not_of(" \t") + 1);

            // Parse both parts as doubles (to handle "3.5/2.0")
            double num = std::stod(numStr);
            double den = std::stod(denStr);

            if (den == 0) {
                std::cerr << "Fraction parser: division by zero in " << str
                          << std::endl;
                return {0, 1};
            }

            // Convert ratio to fraction
            Fraction result = doubleToFraction(num / den);
            return result;
        } catch (const std::exception& e) {
            std::cerr << "Fraction parser error parsing '" << str << "': "
                      << e.what() << std::endl;
            return {0, 1};
        }
    }

    // Case 2: Plain number (integer or decimal)
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
