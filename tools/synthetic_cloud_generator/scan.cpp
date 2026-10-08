#include "generator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace synthetic {
namespace {

std::uint64_t count(double value, const char *name)
{
    // Beyond 2^53 an integer index no longer has an exact double representation.
    if (!std::isfinite(value) || value < 0 || value >= 9007199254740992.0)
        throw Error(std::string("scan: unrepresentable ") + name, 5);
    return static_cast<std::uint64_t>(value);
}

void positive(double value, const char *name)
{
    if (!std::isfinite(value) || value <= 0)
        throw Error(std::string("scan.") + name + ": must be finite and positive");
}

} // namespace

void sampleInterval(Interval interval, double maxLineLength, double pointStep,
                    const std::function<void(double)> &sink)
{
    positive(maxLineLength, "maxLineLength");
    positive(pointStep, "pointStep");
    if (!std::isfinite(interval.min) || !std::isfinite(interval.max) || interval.max < interval.min)
        throw Error("scan: invalid interval", 5);
    if (interval.min == interval.max) {
        sink(interval.min);
        return;
    }
    const double length = interval.max - interval.min;
    const auto segments = count(std::max(1.0, std::ceil(length / maxLineLength)), "segment count");
    double begin = interval.min;
    for (std::uint64_t k = 0; k < segments; ++k) {
        double end = k + 1 == segments ? interval.max
                     : std::min(interval.max, interval.min + static_cast<double>(k + 1) * maxLineLength);
        if (end == begin && end == interval.max)
            break;
        if (!(end > begin))
            throw Error("scan: segment length is not representable", 5);
        const double coordinateRounding = 8 * std::numeric_limits<double>::epsilon()
                                          * std::max({std::abs(begin), std::abs(end), maxLineLength});
        if (end - begin > maxLineLength + coordinateRounding)
            throw Error("scan: maxLineLength cannot be represented at these coordinates", 5);
        const double size = end - begin;
        auto n = count(std::max(1.0, std::ceil(size / pointStep)), "sample count");
        if (size / static_cast<double>(n) > pointStep)
            ++n;
        if (k == 0)
            sink(begin);
        double previous = begin;
        for (std::uint64_t i = 1; i <= n; ++i) {
            const double u = i == n ? end : std::lerp(begin, end, static_cast<double>(i) / static_cast<double>(n));
            if (!(u > previous))
                throw Error("scan: pointStep is below coordinate resolution", 5);
            const double rounding = 8 * std::numeric_limits<double>::epsilon()
                                    * std::max(std::abs(u), std::abs(previous));
            if (u - previous > pointStep + rounding)
                throw Error("scan: sampling exceeds pointStep", 5);
            sink(u);
            previous = u;
        }
        begin = end;
    }
    if (begin != interval.max)
        throw Error("scan: segmentation failed to cover interval", 5);
}

void generate(const PartGeometry &geometry, const ScanScenario &scan, const PointSink &sink)
{
    positive(scan.pointStep, "pointStep");
    positive(scan.lineStep, "lineStep");
    positive(scan.maxLineLength, "maxLineLength");
    const auto bounds = geometryBounds(geometry);
    const bool horizontal = scan.direction == Direction::Horizontal;
    const double origin = horizontal ? bounds.min.y : bounds.min.x;
    const double limit = horizontal ? bounds.max.y : bounds.max.x;
    const auto last = count(std::floor((limit - origin) / scan.lineStep), "scan line count");
    double previous = -std::numeric_limits<double>::infinity();
    for (std::uint64_t i = 0; i <= last; ++i) {
        const double v = origin + static_cast<double>(i) * scan.lineStep;
        if (!std::isfinite(v) || !(v > previous))
            throw Error("scan: lineStep is below coordinate resolution", 5);
        if (v > limit)
            break;
        previous = v;
        for (const auto interval : materialIntervals(geometry, scan.direction, v)) {
            sampleInterval(interval, scan.maxLineLength, scan.pointStep, [&](double u) {
                sink(horizontal ? Point{u, v} : Point{v, u});
            });
        }
    }
}

} // namespace synthetic
