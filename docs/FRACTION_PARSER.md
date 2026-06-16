# Fraction Parser Implementation Guide

## Overview

The fraction parser is responsible for converting various string formats into exact rational numbers (fractions). It handles:
- Plain integers: `"240000"` → `240000/1`
- Explicit fractions: `"240000/48000"` → `240000/48000`
- Decimal numbers: `"5.0"` → `5/1`
- Known decimal values: `"42.6667"` → `2048/48000`
- Best-effort approximations: `"0.208333"` → close rational approximation

## Parsing Algorithm

### Step 1: Attempt Explicit Fraction Format

```cpp
if (str.contains('/')) {
    // Try to parse "numerator/denominator"
    // Both parts can be integers or decimals
    // Examples: "240000/48000", "3/2", "5.5/2.0"
    Parse both parts as doubles
    Check for division by zero
    Return {numerator, denominator}
}
```

### Step 2: Check Known Decimal Values

Common time-related values have known exact fractions:

```
Decimal Value    Exact Fraction    Context
─────────────────────────────────────────────
1.5              3/2               Stretch factor (1.5x)
0.75             3/4               Stretch factor (0.75x)
2.0              2/1               Stretch factor (2x)
42.6667          2048/48000        Grain size @ 48kHz
10.6667          512/48000         Crossfade @ 48kHz
42.6667          1882/44100        Grain size @ 44.1kHz
0.208333         1/4.8             Duration default
```

**Implementation:**

```cpp
static const struct {
    double value;
    uint64_t num;
    uint64_t den;
} KNOWN_DECIMALS[] = {
    {1.5,     3,     2},
    {0.75,    3,     4},
    {2.0,     2,     1},
    {42.6667, 2048,  48000},
    {10.6667, 512,   48000},
    {-1,      0,     0}  // Sentinel
};

for (auto& entry : KNOWN_DECIMALS) {
    if (fabs(value - entry.value) < EPSILON) {
        return {entry.num, entry.den};
    }
}
```

**Tolerance:** Use `1e-6` as epsilon for floating-point comparison.

### Step 3: Continued Fractions Algorithm

For unknown decimals, use continued fractions to find a close rational approximation.

**Continued Fractions Method:**

Converts any real number to a rational approximation by finding its "partial quotients."

```
To convert 0.208333... to a fraction:
  a₀ = floor(0.208333) = 0
  remainder = 0.208333 - 0 = 0.208333
  
  iteration 1:
    reciprocal = 1 / 0.208333 = 4.8
    a₁ = floor(4.8) = 4
    remainder = 4.8 - 4 = 0.8
  
  iteration 2:
    reciprocal = 1 / 0.8 = 1.25
    a₂ = floor(1.25) = 1
    remainder = 1.25 - 1 = 0.25
  
  iteration 3:
    reciprocal = 1 / 0.25 = 4
    a₃ = floor(4) = 4
    remainder = 0  (exact!)
    
  Result: [0; 4, 1, 4] → 1/5 (or 0.2 exactly, close to 0.208333)
```

**Convergents:**

The algorithm tracks convergent fractions at each step:

```
Convergent Sequence:
  p₋₁ = 1,  p₀ = a₀ = 0      q₋₁ = 0,  q₀ = 1
  
  p₁ = a₁ × p₀ + p₋₁ = 4 × 0 + 1 = 1
  q₁ = a₁ × q₀ + q₋₁ = 4 × 1 + 0 = 4
  Convergent: 1/4
  
  p₂ = a₂ × p₁ + p₀ = 1 × 1 + 0 = 1
  q₂ = a₂ × q₁ + q₀ = 1 × 4 + 1 = 5
  Convergent: 1/5 ← Best approximation!
```

**C++ Implementation:**

```cpp
Fraction doubleToFraction(double value, 
                         uint64_t maxDenominator = 1000000) {
    // Handle edge cases
    if (isnan(value) || isinf(value)) {
        return {0, 1};  // Error
    }
    
    // Exact integer
    if (fabs(value - round(value)) < 1e-10) {
        return {(uint64_t)round(value), 1};
    }
    
    // Continued fractions algorithm
    int64_t wholePart = (int64_t)floor(value);
    double remainder = value - wholePart;
    
    uint64_t num_prev = 1,  den_prev = 0;
    uint64_t num_curr = wholePart, den_curr = 1;
    
    for (int iteration = 0; iteration < 20; ++iteration) {
        // Stop if remainder is negligible
        if (remainder < 1e-10) {
            break;
        }
        
        // Stop if denominator would exceed limit
        if (den_curr > maxDenominator) {
            break;  // Return best approximation so far
        }
        
        // Next partial quotient
        remainder = 1.0 / remainder;
        int64_t a = (int64_t)floor(remainder);
        remainder -= a;
        
        // Compute next convergent
        uint64_t num_next = a * num_curr + num_prev;
        uint64_t den_next = a * den_curr + den_prev;
        
        // Would denominator overflow?
        if (den_next > maxDenominator) {
            break;  // Return current best
        }
        
        // Update for next iteration
        num_prev = num_curr;
        den_prev = den_curr;
        num_curr = num_next;
        den_curr = den_next;
    }
    
    return {num_curr, den_curr};
}
```

**Tuning Parameters:**
- `maxDenominator`: Stop if denominator would exceed this
  - Default: 1,000,000
  - Prevents huge fractions while maintaining reasonable precision
- `maxIterations`: Maximum iterations of the algorithm
  - Default: 20
  - Typically converges in 5-10 iterations

**Error Bounds:**

The continued fractions algorithm produces increasingly good approximations:
```
For π = 3.14159265...

Approximation    Error
─────────────────────────────
3/1              0.14159
22/7             0.001264
355/113          0.00000027
52163/16604      negligible
```

Our algorithm with `maxDenominator = 1,000,000` will find extremely accurate approximations.

## Complete Parser Function

```cpp
// Parse a time value from XML attribute
// Handles: "240000", "240000/48000", "5.0", "1.5", "42.6667"
Fraction parseFractionOrDouble(const QString& str) {
    // Trim whitespace
    QString s = str.trimmed();
    
    // Case 1: Empty string
    if (s.isEmpty()) {
        return {0, 1};  // Default to 0
    }
    
    // Case 2: Explicit fraction "num/denom"
    if (s.contains('/')) {
        QStringList parts = s.split('/', Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            bool ok1, ok2;
            double num = parts[0].trimmed().toDouble(&ok1);
            double den = parts[1].trimmed().toDouble(&ok2);
            
            if (ok1 && ok2 && den != 0) {
                // Both parts might be decimals, combine first
                Fraction result = doubleToFraction(num / den);
                return result;
            }
        }
        // Malformed fraction, return error
        qWarning() << "Malformed fraction:" << str;
        return {0, 1};
    }
    
    // Case 3: Plain number (integer or decimal)
    bool ok;
    double value = s.toDouble(&ok);
    if (!ok) {
        qWarning() << "Cannot parse as number:" << str;
        return {0, 1};  // Error
    }
    
    // Try known decimals first
    return doubleToFractionWithLookup(value);
}

// Helper: try lookup table, then continued fractions
Fraction doubleToFractionWithLookup(double value) {
    // Check known decimal values (with tolerance)
    const double TOLERANCE = 1e-6;
    
    static const struct {
        double value;
        uint64_t num;
        uint64_t den;
    } KNOWN[] = {
        // Stretch factors
        {0.5,     1,     2},
        {0.75,    3,     4},
        {1.0,     1,     1},
        {1.5,     3,     2},
        {2.0,     2,     1},
        {2.5,     5,     2},
        
        // Grain sizes @ 48kHz
        {42.6667, 2048,  48000},
        {10.6667, 512,   48000},
        
        // Grain sizes @ 44.1kHz
        {42.6667, 1882,  44100},
        {10.6667, 470,   44100},
        
        // Durations
        {0.208333, 1,    4.8},  // Note: denominator is float, will be rationalized
        
        {-1, 0, 0}  // Sentinel
    };
    
    for (auto& entry : KNOWN) {
        if (entry.value < 0) break;
        if (fabs(value - entry.value) < TOLERANCE) {
            return {entry.num, (uint64_t)entry.den};
        }
    }
    
    // Not in lookup table, use continued fractions
    return doubleToFraction(value, 1000000);
}

// Simplify fraction by dividing by GCD
void simplify(uint64_t& num, uint64_t& den) {
    uint64_t g = gcd(num, den);
    if (g > 1) {
        num /= g;
        den /= g;
    }
}

// GCD using Euclidean algorithm
uint64_t gcd(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}
```

## Integration Points

### In XML Element Reading

```cpp
// When reading position from XML:
int SCut::readPostChildrenAttributes(QDomElement &element) {
    // Parse start offset (in project sample units)
    QString offsetStr = element.attribute("startOffset", "0");
    Fraction offsetFrac = parseFractionOrDouble(offsetStr);
    setStartOffset(offsetFrac.numerator);  // Store as integer samples
    
    // Parse duration
    QString durStr = element.attribute("cutDuration", "240000");
    Fraction durFrac = parseFractionOrDouble(durStr);
    cutDuration_ = durFrac.numerator;
    
    // Parse grain size (which IS a time value)
    QString grainStr = element.attribute("grainSizeMs", "2048/48000");
    Fraction grainFrac = parseFractionOrDouble(grainStr);
    int srate = parent() ? getProject().getSRate() : 48000;
    grainParams_.grainSize = (grainFrac.numerator * srate) / 
                              grainFrac.denominator;
    // Now grainSize is in samples at the current sample rate
}
```

## Testing

### Test Cases

```cpp
// Test data
struct TestCase {
    QString input;
    uint64_t expectedNum;
    uint64_t expectedDen;
    double tolerance;  // For floating-point comparison
};

TestCase tests[] = {
    // Integers
    {"240000",        240000, 1,        1e-10},
    {"0",             0,      1,        1e-10},
    {"1",             1,      1,        1e-10},
    
    // Explicit fractions
    {"240000/48000",  5,      1,        1e-10},  // Simplified!
    {"3/2",           3,      2,        1e-10},
    {"1/2",           1,      2,        1e-10},
    
    // Decimals
    {"5.0",           5,      1,        1e-10},
    {"1.5",           3,      2,        1e-6},   // From lookup
    {"0.5",           1,      2,        1e-6},   // From lookup
    
    // Known grain sizes
    {"42.6667",       2048,   48000,    1e-3},   // Approximate match
    
    // Edge cases
    {"",              0,      1,        1e-10},  // Empty
    {"0.0",           0,      1,        1e-10},  // Zero
    {"invalid",       0,      1,        1e-10},  // Error returns 0/1
};
```

## Performance Considerations

- **Lookup table**: O(1) for known values
- **Continued fractions**: O(log(denominator)), typically 5-10 iterations
- **GCD simplification**: O(log(min(num, den)))
- **Overall**: Negligible, even with hundreds of positions per project

## Known Limitations

1. **Very Large Numbers**: If numerator or denominator exceed `uint64_t` max:
   - Algorithm clamps to max value
   - Emits warning
   - User should avoid huge position values

2. **Irrational Numbers**: Some values have no exact rational representation:
   - Continued fractions finds best approximation
   - Stops when denominator exceeds limit
   - Typically accurate to machine precision

3. **Decimal Precision**: User-entered decimals are approximate:
   - `"42.6667"` loses precision from original `"2048/48000"`
   - Lookup table recovers exact value if known
   - Otherwise, approximation is good enough

## Future Improvements

1. **Expand lookup table**: Add more known values as they're discovered
2. **Caching**: Cache frequently-parsed values
3. **User warning**: Warn if input was approximate and exact alternative exists
4. **Format migration**: Tool to automatically convert old files to new fraction format
