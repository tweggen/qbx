#ifndef _TW_AUTOMATIONCURVE_H_
#define _TW_AUTOMATIONCURVE_H_

#include <cstdint>
#include <memory>
#include <vector>

/**
 * twAutomationCurve — an immutable, sorted breakpoint curve in FRAMES
 * (proposal 36 D5).
 *
 * The snapshot a consuming component is handed under its mutex and reads at
 * freeze time, exactly like a cut snapshot: the UI edits the model, the model
 * builds a new curve, the component swaps the pointer. Nothing here allocates
 * or locks while a page is being rendered — `fillRamp` writes into the caller's
 * buffer.
 *
 * A segment's SHAPE belongs to its LEFT point (`points[i].shape` governs
 * [points[i].frame, points[i+1].frame)) — the convention every DAW's curve
 * editor draws. Before the first point the curve holds the first value; after
 * the last it holds the last; an empty curve is `defaultValue`.
 */

enum class twCurveShape : uint8_t {
    Step = 0,    // hold the left value until the next point
    Linear = 1,  // straight line
    Exp = 2      // tension-shaped; tension 0 IS linear (see valueAt)
};

struct twCurvePoint {
    int64_t      frame = 0;
    double       value = 0.0;
    twCurveShape shape = twCurveShape::Linear;
    double       tension = 0.0;   // Exp only; >0 bends late, <0 bends early

    bool operator==(const twCurvePoint &o) const {
        return frame == o.frame && value == o.value && shape == o.shape
            && tension == o.tension;
    }
};

class twAutomationCurve
{
public:
    twAutomationCurve() = default;
    explicit twAutomationCurve(std::vector<twCurvePoint> points,
                               double defaultValue = 0.0);

    const std::vector<twCurvePoint> &points() const { return points_; }
    bool   empty() const { return points_.empty(); }
    double defaultValue() const { return default_; }

    /**
     * The value at `frame`.
     *
     * Closed form of the Exp shape, over u = (f − f0)/(f1 − f0) ∈ [0,1):
     *
     *   |tension| < 1e-12 :  v = v0 + (v1 − v0)·u                (linear)
     *   otherwise         :  v = v0 + (v1 − v0)·(e^{k·u} − 1)/(e^{k} − 1)
     *
     * — continuous in the tension at 0, monotone, and exactly v0 at u = 0 and
     * v1 at u → 1, which is what makes the "matches the closed form to 1e-12"
     * assertion meaningful rather than a restatement of the code.
     */
    double valueAt(int64_t frame) const;

    /**
     * `n` consecutive values starting at `startFrame`, one per frame, into
     * `dst`. Identical to n calls of valueAt (asserted in events_test); it
     * exists only so a freeze walks the breakpoints once instead of
     * binary-searching per sample.
     */
    void fillRamp(double *dst, int64_t startFrame, int64_t n) const;

private:
    size_t segmentFor_(int64_t frame) const;   // index of the point at/below frame

    std::vector<twCurvePoint> points_;
    double default_ = 0.0;
};

#endif // _TW_AUTOMATIONCURVE_H_
