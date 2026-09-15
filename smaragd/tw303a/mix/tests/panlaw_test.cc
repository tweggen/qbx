// tw/mix pan law test (proposal 49 M0, AC0.1): twPanLaw against its closed
// forms. Pure arithmetic -- no component, no page, no environment.
//
// What bites here, and why each check exists:
//   - centre is {1, 1} BIT-EXACT, not merely close: D5's byte identity rests
//     on pan 0 doing no arithmetic, and a law that returned cos(0) through a
//     multiply would still be 1.0 but would invite a caller to multiply;
//   - the far side at +-1 is EXACTLY 0.0 (cos(pi/2) is 6.1e-17, which leaves
//     an audible-in-principle residue and fails a "channel is silent" gate);
//   - p = 0.5 gives cos(pi/4) on the far side (-3.01 dB), which separates the
//     cos sweep from a linear one (0.5, -6.02 dB);
//   - the near side is held at unity everywhere (the balance family, not the
//     compensated one that rises to +3 dB and can clip);
//   - monotone over the control's 201 ticks, and mirrored.
#include "tw/mix/twpanlaw.h"

#include <cmath>
#include <cstdio>
#include <limits>

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { printf("ok   %s\n", msg); }                             \
        else      { printf("FAIL %s\n", msg); ++failures; }                 \
    } while (0)

static const double kPi = 3.141592653589793238462643383279502884;

int main()
{
    // --- centre --------------------------------------------------------------
    {
        const twPanGains g = twPanLaw(0.0);
        CHECK(g.l == 1.0 && g.r == 1.0, "twPanLaw(0) == {1, 1} bit-exact");
        const twPanGains n = twPanLaw(-0.0);
        CHECK(n.l == 1.0 && n.r == 1.0, "twPanLaw(-0.0) == {1, 1}");
    }

    // --- hard pan --------------------------------------------------------------
    {
        const twPanGains R = twPanLaw(1.0);
        CHECK(R.l == 0.0 && R.r == 1.0, "twPanLaw(+1): far side (l) is exactly 0.0, near side 1");
        const twPanGains L = twPanLaw(-1.0);
        CHECK(L.r == 0.0 && L.l == 1.0, "twPanLaw(-1): far side (r) is exactly 0.0, near side 1");
    }

    // --- the cos sweep ---------------------------------------------------------
    {
        const double want = std::cos(kPi / 4.0);
        const twPanGains h = twPanLaw(0.5);
        CHECK(std::fabs(h.l - want) <= 1e-15 && h.r == 1.0,
              "twPanLaw(0.5).l == cos(pi/4) to 1e-15 (the -3.01 dB point, not linear 0.5)");
        const twPanGains q = twPanLaw(0.75);
        CHECK(std::fabs(q.l - std::cos(0.375 * kPi)) <= 1e-15,
              "twPanLaw(0.75).l == cos(0.375 pi) (M1's closed form, AC1.1)");
    }

    // --- mirror, near side unity, monotone over the 201 control ticks ----------
    {
        bool mirrored = true, nearUnity = true, monotone = true, bounded = true;
        double prevL = 2.0, prevR = -1.0;
        for (int t = -100; t <= 100; ++t) {
            const double p = t / 100.0;
            const twPanGains g = twPanLaw(p);
            const twPanGains m = twPanLaw(-p);
            if (g.l != m.r || g.r != m.l) mirrored = false;
            if ((p >= 0.0 && g.r != 1.0) || (p <= 0.0 && g.l != 1.0)) nearUnity = false;
            if (g.l > prevL || g.r < prevR) monotone = false;
            if (!(g.l >= 0.0 && g.l <= 1.0 && g.r >= 0.0 && g.r <= 1.0)) bounded = false;
            prevL = g.l;
            prevR = g.r;
        }
        CHECK(mirrored, "twPanLaw(-p) mirrors twPanLaw(p) over 201 ticks");
        CHECK(nearUnity, "the near side is held at exactly 1.0 (balance, never above unity)");
        CHECK(monotone, "l is non-increasing and r non-decreasing across -1..1");
        CHECK(bounded, "every gain lies in [0, 1]");
    }

    // --- inputs a caller should never pass, and must not hurt -----------------
    {
        const twPanGains nan = twPanLaw(std::numeric_limits<double>::quiet_NaN());
        CHECK(nan.l == 1.0 && nan.r == 1.0, "NaN is centre (no NaN sample can come out)");
        const twPanGains over = twPanLaw(3.0);
        CHECK(over.l == 0.0 && over.r == 1.0, "p > 1 is hard right");
        const twPanGains under = twPanLaw(-7.5);
        CHECK(under.r == 0.0 && under.l == 1.0, "p < -1 is hard left");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
