#include "tw/core/twtimemap.h"
#include <iostream>
#include <string>
#include <vector>

// Proposal 18 Phase 4: property tests for the exact interval maps.

static int passCount = 0, failCount = 0;

static void check(const std::string& name, bool ok) {
    if (ok) { passCount++; std::cout << "OK   " << name << std::endl; }
    else    { failCount++; std::cout << "FAIL " << name << std::endl; }
}

static void testAffine() {
    std::cout << "\n--- twAffineMap ---" << std::endl;

    // Awkward scale (a real-world stretch ratio) and offset
    twAffineMap m(Fraction(44543, 48000), Fraction(157954));

    // map/inverse exact roundtrip
    Fraction x(96000);
    check("affine inverse(map(x)) == x", m.inverse(m.map(x)) == x);
    Fraction y(1234567, 89);
    check("affine map(inverse(y)) == y", m.map(m.inverse(y)) == y);

    // composition == sequential application
    twAffineMap inner(Fraction(3, 2), Fraction(-480));
    twAffineMap comp = m.composedWith(inner);
    check("composed == outer(inner(x))",
          comp.map(x) == m.map(inner.map(x)));
    check("composition is affine", comp.isAffine());

    // interval maps to exactly one segment covering the input
    auto segs = m.mapInterval(Fraction(100), Fraction(500));
    check("affine mapInterval single segment", segs.size() == 1);
    check("affine segment covers input",
          segs[0].parentStart == Fraction(100) &&
          segs[0].length == Fraction(500) &&
          segs[0].childStart == m.map(Fraction(100)));
}

static void testLoopPointMap() {
    std::cout << "\n--- twLoopMap points ---" << std::endl;

    twLoopMap loop(Fraction(157954), Fraction(96000));

    check("first iteration maps inside base window",
          loop.map(Fraction(1000)) == Fraction(158954));
    check("wrap: pos len+1000 maps like pos 1000",
          loop.map(Fraction(96000 + 1000)) == loop.map(Fraction(1000)));
    check("boundary: pos == len wraps to base",
          loop.map(Fraction(96000)) == Fraction(157954));
    check("canonical inverse is first iteration",
          loop.inverse(loop.map(Fraction(1000))) == Fraction(1000));

    // Rational loop length also works (future exact windows)
    twLoopMap frac(Fraction(0), Fraction(96000, 7));
    check("rational loop length wraps exactly",
          frac.map(Fraction(96000, 7) + Fraction(5)) == Fraction(5));
}

static void testLoopTiling() {
    std::cout << "\n--- twLoopMap tiling ---" << std::endl;

    twLoopMap loop(Fraction(157954), Fraction(96000));

    // A block spanning several wraps: segments must tile the input exactly
    // (this is the twLoopReader chunk loop, extracted)
    Fraction start(50000), len(300000);
    auto segs = loop.mapInterval(start, len);

    bool contiguous = true, inWindow = true;
    Fraction pos = start, total(0);
    for (const auto& s : segs) {
        if (s.parentStart != pos) contiguous = false;
        if (s.childStart < loop.base() ||
            !(s.childStart + s.length <= loop.base() + loop.length()))
            inWindow = false;
        pos = pos + s.length;
        total = total + s.length;
    }
    check("loop segments contiguous (no gaps/overlaps)", contiguous);
    check("loop segment lengths sum to input length", total == len);
    check("every segment inside the loop window", inWindow);
    check("expected wrap count", segs.size() == 4);  // 46000 + 96000 + 96000 + 62000

    // Each segment's child start equals map() of its parent start
    bool pointsAgree = true;
    for (const auto& s : segs)
        if (loop.map(s.parentStart) != s.childStart) pointsAgree = false;
    check("segment childStart == map(parentStart)", pointsAgree);

    // Integer equivalence with the historical modulo arithmetic
    bool sameAsModulo = true;
    for (int64_t p = 0; p < 4 * 96000; p += 12345) {
        int64_t expect = 157954 + (p % 96000);
        if (loop.map(Fraction(p)) != Fraction(expect)) sameAsModulo = false;
    }
    check("integer positions match legacy modulo", sameAsModulo);
}

static void testLoopPreimages() {
    std::cout << "\n--- twLoopMap preimages ---" << std::endl;

    twLoopMap loop(Fraction(1000), Fraction(500));

    // Child window [1100, 1200) has one image per tile; parent range covers
    // exactly 3 tiles => 3 preimages
    auto pre = loop.preimagesWithin(Fraction(1100), Fraction(100),
                                    Fraction(0), Fraction(1500));
    check("one preimage per covered tile", pre.size() == 3);
    bool eachMapsBack = true;
    for (const auto& s : pre)
        if (loop.map(s.parentStart) != s.childStart) eachMapsBack = false;
    check("preimages map back into the child window", eachMapsBack);

    // Range clipping: a range starting mid-tile clips the first preimage
    auto clipped = loop.preimagesWithin(Fraction(1100), Fraction(100),
                                        Fraction(150), Fraction(500));
    check("range-clipped preimages stay within range",
          !clipped.empty() && clipped.front().parentStart >= Fraction(150));

    // A child window outside the loop segment has no preimages
    auto none = loop.preimagesWithin(Fraction(0), Fraction(500),
                                     Fraction(0), Fraction(5000));
    check("child window outside the loop segment -> none", none.empty());
}

int main() {
    std::cout << "TWTIMEMAP PROPERTY TEST SUITE" << std::endl;

    testAffine();
    testLoopPointMap();
    testLoopTiling();
    testLoopPreimages();

    std::cout << "\npassed " << passCount << ", failed " << failCount << std::endl;
    return failCount > 0 ? 1 : 0;
}
