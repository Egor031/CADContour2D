#include "generator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <type_traits>

namespace synthetic {
namespace {

Point xy(double u, double v, Direction d) { return d == Direction::Horizontal ? Point{u, v} : Point{v, u}; }
Point uv(Point p, Direction d) { return d == Direction::Horizontal ? p : Point{p.y, p.x}; }
bool contains(Bounds b, Point p)
{
    return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y;
}
void positive(double x, const char *name)
{
    if (!std::isfinite(x) || x <= 0) throw Error(std::string("scan.") + name + ": must be finite and positive");
}
void validRegion(Bounds b)
{
    if (!std::isfinite(b.min.x) || !std::isfinite(b.min.y) || !std::isfinite(b.max.x) || !std::isfinite(b.max.y)
        || !(b.max.x > b.min.x) || !(b.max.y > b.min.y)
        || !std::isfinite(b.max.x - b.min.x) || !std::isfinite(b.max.y - b.min.y))
        throw Error("scan.region: invalid rectangle");
}
std::int64_t index(double x)
{
    if (!std::isfinite(x) || std::abs(x) >= 9007199254740992.0)
        throw Error("scan: unrepresentable grid/segment count", 5);
    return static_cast<std::int64_t>(x);
}
std::uint64_t mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
double uniform(std::mt19937_64 &rng) { return static_cast<double>(rng() >> 11) * 0x1.0p-53; }
bool onBoundary(const std::vector<Interval> &boundary, double u)
{
    return std::any_of(boundary.begin(), boundary.end(), [u](Interval i) { return u >= i.min && u <= i.max; });
}
bool standalone(const Defect &d)
{
    return std::holds_alternative<OutsideGridCloud>(d) || std::holds_alternative<ExtraTableFragment>(d);
}

struct MissingState { std::uint32_t remaining = 0; bool keepNext = false; };

void artifactPass(const PartGeometry &g, const ScanScenario &s, const ScanPass &pass,
                  std::size_t passIndex, const PointSink &sink)
{
    const auto bounds = geometryBounds(g);
    Bounds coverage = bounds;
    if (pass.region) {
        coverage.min.x = std::max(bounds.min.x, pass.region->min.x);
        coverage.min.y = std::max(bounds.min.y, pass.region->min.y);
        coverage.max.x = std::min(bounds.max.x, pass.region->max.x);
        coverage.max.y = std::min(bounds.max.y, pass.region->max.y);
        if (coverage.max.x < coverage.min.x || coverage.max.y < coverage.min.y) return;
    }
    const auto lo = uv(coverage.min, pass.direction), hi = uv(coverage.max, pass.direction);
    const double origin = uv(bounds.min, pass.direction).y + pass.lineOffset;
    const auto firstLine = index(std::ceil((lo.y - origin) / s.lineStep));
    const auto lastLine = index(std::floor((hi.y - origin) / s.lineStep));
    std::vector<std::mt19937_64> random;
    for (std::size_t i = 0; i < s.defects.size(); ++i) random.emplace_back(randomStreamSeed(s, passIndex, i));
    double previousLine = -std::numeric_limits<double>::infinity();
    for (auto line = firstLine; line <= lastLine; ++line) {
        const double v = origin + static_cast<double>(line) * s.lineStep;
        if (!std::isfinite(v) || !(v > previousLine)) throw Error("scan: lineStep below coordinate resolution", 5);
        previousLine = v;
        if (v < lo.y || v > hi.y) continue;
        std::vector<std::vector<Interval>> boundaries(s.defects.size());
        for (std::size_t j = 0; j < s.defects.size(); ++j) {
            if (const auto *d = std::get_if<JaggedBoundary>(&s.defects[j]))
                boundaries[j] = contourBoundaryIntervals(d->inner ? g.inner[d->innerIndex] : g.outer, pass.direction, v);
        }
        for (const auto original : materialIntervals(g, pass.direction, v)) {
            const Interval clipped{std::max(original.min, lo.x), std::min(original.max, hi.x)};
            if (clipped.max < clipped.min) continue;
            const auto segments = index(std::max(1.0, std::ceil((clipped.max - clipped.min) / s.maxLineLength)));
            double begin = clipped.min;
            std::optional<Point> previousEnd;
            for (std::int64_t k = 0; k < segments; ++k) {
                const double end = k + 1 == segments ? clipped.max
                    : std::min(clipped.max, clipped.min + static_cast<double>(k + 1) * s.maxLineLength);
                const bool contact = clipped.min == clipped.max;
                if (!contact && end == begin && end == clipped.max) break;
                if (!contact && !(end > begin)) throw Error("scan: segment length is not representable", 5);
                const double rounding = 8 * std::numeric_limits<double>::epsilon()
                    * std::max({std::abs(begin), std::abs(end), s.maxLineLength});
                if (end - begin > s.maxLineLength + rounding) throw Error("scan: maxLineLength is not representable", 5);
                double a = begin, b = end;
                const auto jaggedOffset = [&](double u) {
                    double displacement = 0;
                    for (std::size_t j = 0; j < s.defects.size(); ++j) {
                        const auto *d = std::get_if<JaggedBoundary>(&s.defects[j]);
                        if (!d || !contains(d->region, xy(u, v, pass.direction)) || !onBoundary(boundaries[j], u)) continue;
                        const double phase = (v - uv(d->region.min, pass.direction).y) / d->toothStep;
                        if (!std::isfinite(phase) || std::abs(phase) >= 9007199254740992.0)
                            throw Error("scan: unresolved jagged tooth phase", 5);
                        const double f = phase - std::floor(phase);
                        displacement += d->amplitude * (1 - std::abs(2 * f - 1));
                        if (d->randomAmplitude != 0) displacement += d->randomAmplitude * uniform(random[j]);
                    }
                    return displacement;
                };
                // Only original contour endpoints are modified, never ROI or continuation seams.
                if (begin == original.min) a -= jaggedOffset(begin);
                if (!contact && end == original.max) b += jaggedOffset(end);
                if (contact) b = a;
                const double du = pass.shift.longitudinal + static_cast<double>(k) * pass.continuation.longitudinal;
                const double dv = pass.shift.transverse + static_cast<double>(k) * pass.continuation.transverse;
                const Point measuredFirst = xy(a + du, v + dv, pass.direction);
                const Point measuredLast = xy(b + du, v + dv, pass.direction);
                validateOutputSegment(measuredFirst, measuredLast);
                if (!contact && !(b + du > a + du)) throw Error("scan: positive measured segment is not representable", 5);
                std::vector<MissingState> states(s.defects.size());
                sampleInterval({a + du, b + du}, std::numeric_limits<double>::max(), s.pointStep, [&](double u) {
                    const Point p = xy(u, v + dv, pass.direction);
                    const bool endpoint = u == a + du || u == b + du;
                    bool missing = false;
                    for (std::size_t j = 0; j < s.defects.size(); ++j) {
                        const auto *d = std::get_if<MissingPoints>(&s.defects[j]);
                        if (!d) continue;
                        auto &state = states[j];
                        if (endpoint || (d->region && !contains(*d->region, p))) { state = {}; continue; }
                        if (state.remaining != 0) { --state.remaining; missing = true; }
                        else if (state.keepNext) state.keepNext = false;
                        else if (uniform(random[j]) < d->probability) {
                            state.remaining = d->minRunLength + static_cast<std::uint32_t>(random[j]() % (d->maxRunLength - d->minRunLength + 1)) - 1;
                            state.keepNext = true;
                            missing = true;
                        }
                    }
                    const bool seam = u == a + du && previousEnd && p.x == previousEnd->x && p.y == previousEnd->y;
                    if (!missing && !seam) sink(p);
                });
                previousEnd = measuredLast;
                begin = end;
            }
            if (begin != clipped.max) throw Error("scan: incomplete interval segmentation", 5);
        }
    }
}

} // namespace

std::vector<ScanPass> scanPasses(const ScanScenario &scan)
{
    return scan.passes.empty() ? std::vector<ScanPass>{{scan.direction, std::nullopt, 0, {}, {}}} : scan.passes;
}

bool cleanPass(const ScanScenario &scan, const ScanPass &pass)
{
    return !pass.region && pass.lineOffset == 0 && pass.shift.longitudinal == 0 && pass.shift.transverse == 0
        && pass.continuation.longitudinal == 0 && pass.continuation.transverse == 0
        && std::all_of(scan.defects.begin(), scan.defects.end(), standalone);
}

std::uint64_t randomStreamSeed(const ScanScenario &scan, std::size_t passIndex, std::size_t defectIndex)
{
    const auto type = scan.defects.at(defectIndex).index();
    std::uint64_t ordinal = 0;
    for (std::size_t i = 0; i < defectIndex; ++i) if (scan.defects[i].index() == type) ++ordinal;
    return mix(mix(scan.seed) ^ mix(passIndex) ^ mix(0xdefec700ULL + type) ^ mix(0x51de0000ULL + ordinal));
}

void validateScenario(const PartGeometry &geometry, const ScanScenario &scan)
{
    positive(scan.pointStep, "pointStep"); positive(scan.lineStep, "lineStep"); positive(scan.maxLineLength, "maxLineLength");
    for (const auto &pass : scanPasses(scan)) {
        if (pass.region) validRegion(*pass.region);
        for (double x : {pass.lineOffset, pass.shift.longitudinal, pass.shift.transverse,
                         pass.continuation.longitudinal, pass.continuation.transverse})
            if (!std::isfinite(x)) throw Error("scan.passes: nonfinite shift");
    }
    for (std::size_t i = 0; i < scan.defects.size(); ++i) {
        try {
            std::visit([&](const auto &d) {
                using T = std::decay_t<decltype(d)>;
                if constexpr (std::is_same_v<T, MissingPoints>) {
                    if (d.region) validRegion(*d.region);
                    if (!std::isfinite(d.probability) || d.probability < 0 || d.probability > 1
                        || d.minRunLength < 1 || d.maxRunLength < d.minRunLength || d.maxRunLength > 3)
                        throw Error("invalid MissingPoints parameters");
                } else {
                    validRegion(d.region);
                    if constexpr (std::is_same_v<T, JaggedBoundary>) {
                        if (d.inner && d.innerIndex >= geometry.inner.size()) throw Error("innerIndex does not exist");
                        positive(d.amplitude, "amplitude"); positive(d.toothStep, "toothStep");
                        if (!std::isfinite(d.randomAmplitude) || d.randomAmplitude < 0) throw Error("invalid randomAmplitude");
                    } else {
                        validateOutsideRegion(geometry, d.region);
                        if constexpr (std::is_same_v<T, OutsideGridCloud>) {
                            positive(d.pointStepX, "pointStepX"); positive(d.pointStepY, "pointStepY");
                        }
                    }
                }
            }, scan.defects[i]);
        } catch (const Error &e) {
            throw Error("scan.defects[" + std::to_string(i) + "]: " + e.what(), e.exitCode);
        }
    }
}

void generatePass(const PartGeometry &geometry, const ScanScenario &scan,
                  std::size_t passIndex, const PointSink &sink)
{
    const auto pass = scanPasses(scan).at(passIndex);
    if (cleanPass(scan, pass)) generateCleanPass(geometry, scan, pass.direction, sink);
    else artifactPass(geometry, scan, pass, passIndex, sink);
}

void generateStandalone(const ScanScenario &scan, std::size_t defectIndex, const PointSink &sink)
{
    const auto &defect = scan.defects.at(defectIndex);
    if (const auto *d = std::get_if<OutsideGridCloud>(&defect)) {
        const auto nx = index(std::floor((d->region.max.x - d->region.min.x) / d->pointStepX));
        const auto ny = index(std::floor((d->region.max.y - d->region.min.y) / d->pointStepY));
        double previousY = -std::numeric_limits<double>::infinity();
        for (std::int64_t j = 0; j <= ny; ++j) {
            const double y = d->region.min.y + static_cast<double>(j) * d->pointStepY;
            if (!(y > previousY)) throw Error("scan: unresolved outside grid Y step", 5);
            previousY = y;
            if (y > d->region.max.y) break;
            double previousX = -std::numeric_limits<double>::infinity();
            for (std::int64_t i = 0; i <= nx; ++i) {
                const double x = d->region.min.x + static_cast<double>(i) * d->pointStepX;
                if (!(x > previousX)) throw Error("scan: unresolved outside grid X step", 5);
                previousX = x;
                if (x <= d->region.max.x) sink({x, y});
            }
        }
    } else if (const auto *table = std::get_if<ExtraTableFragment>(&defect)) {
        const auto lo = uv(table->region.min, scan.direction), hi = uv(table->region.max, scan.direction);
        const auto last = index(std::floor((hi.y - lo.y) / scan.lineStep));
        double previous = -std::numeric_limits<double>::infinity();
        for (std::int64_t i = 0; i <= last; ++i) {
            const double v = lo.y + static_cast<double>(i) * scan.lineStep;
            if (!(v > previous)) throw Error("scan: unresolved table lineStep", 5);
            previous = v;
            if (v > hi.y) break;
            validateOutputSegment(xy(lo.x, v, scan.direction), xy(hi.x, v, scan.direction));
            sampleInterval({lo.x, hi.x}, scan.maxLineLength, scan.pointStep, [&](double u) { sink(xy(u, v, scan.direction)); });
        }
    }
}

} // namespace synthetic
