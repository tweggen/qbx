#include "tw/events/twautomationcurve.h"

#include <algorithm>
#include <cmath>

twAutomationCurve::twAutomationCurve(std::vector<twCurvePoint> points,
                                     double defaultValue)
    : points_(std::move(points)), default_(defaultValue)
{
    std::stable_sort(points_.begin(), points_.end(),
        [](const twCurvePoint &a, const twCurvePoint &b) {
            return a.frame < b.frame;
        });
}

// Index of the last point with frame <= f, or npos when f is before the first.
size_t twAutomationCurve::segmentFor_(int64_t f) const
{
    if (points_.empty() || f < points_.front().frame) return (size_t)-1;
    size_t lo = 0, hi = points_.size();
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (points_[mid].frame <= f) lo = mid; else hi = mid;
    }
    // Several points may share a frame: the LAST one wins (an instant jump).
    while (lo + 1 < points_.size() && points_[lo + 1].frame <= f) ++lo;
    return lo;
}

static double shapeInterp(const twCurvePoint &a, const twCurvePoint &b,
                          int64_t frame)
{
    const int64_t span = b.frame - a.frame;
    if (span <= 0) return b.value;
    const double u = (double)(frame - a.frame) / (double)span;
    switch (a.shape) {
    case twCurveShape::Step:
        return a.value;
    case twCurveShape::Linear:
        return a.value + (b.value - a.value) * u;
    case twCurveShape::Exp: {
        const double k = a.tension;
        if (std::fabs(k) < 1e-12) return a.value + (b.value - a.value) * u;
        const double w = (std::exp(k * u) - 1.0) / (std::exp(k) - 1.0);
        return a.value + (b.value - a.value) * w;
    }
    }
    return a.value;
}

double twAutomationCurve::valueAt(int64_t frame) const
{
    if (points_.empty()) return default_;
    const size_t i = segmentFor_(frame);
    if (i == (size_t)-1) return points_.front().value;   // before the first
    if (i + 1 >= points_.size()) return points_[i].value; // at/after the last
    return shapeInterp(points_[i], points_[i + 1], frame);
}

void twAutomationCurve::fillRamp(double *dst, int64_t startFrame, int64_t n) const
{
    if (!dst || n <= 0) return;
    if (points_.empty()) {
        for (int64_t i = 0; i < n; ++i) dst[i] = default_;
        return;
    }
    size_t i = segmentFor_(startFrame);
    for (int64_t k = 0; k < n; ++k) {
        const int64_t f = startFrame + k;
        // Advance the cursor rather than re-searching; the "last point sharing
        // a frame wins" rule of segmentFor_ has to hold here too.
        if (i == (size_t)-1) {
            if (f >= points_.front().frame) i = 0;
        }
        if (i != (size_t)-1) {
            while (i + 1 < points_.size() && points_[i + 1].frame <= f) ++i;
        }
        if (i == (size_t)-1)                dst[k] = points_.front().value;
        else if (i + 1 >= points_.size())   dst[k] = points_[i].value;
        else                                dst[k] = shapeInterp(points_[i],
                                                                 points_[i + 1], f);
    }
}
