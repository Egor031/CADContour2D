#include "generator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <string>

namespace synthetic {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double tau = 2 * pi;
constexpr double angleTolerance = 64 * std::numeric_limits<double>::epsilon();
using Primitive = std::variant<Line, Arc, Circle>;

Point add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
Point sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
Point mul(Point a, double s) { return {a.x * s, a.y * s}; }
double dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }
double cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
double length(Point a) { return std::hypot(a.x, a.y); }
bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }

double coordinateTolerance(Point a, Point b, double localScale = 0)
{
    return 64 * std::numeric_limits<double>::epsilon()
           * std::max({std::abs(a.x), std::abs(a.y), std::abs(b.x), std::abs(b.y),
                       length(sub(a, b)), localScale, std::numeric_limits<double>::min()});
}

bool near(Point a, Point b, double localScale = 0)
{
    return length(sub(a, b)) <= coordinateTolerance(a, b, localScale);
}

double segmentScale(const Segment &s)
{
    if (const auto *l = std::get_if<Line>(&s))
        return length(sub(l->end, l->start));
    return std::get<Arc>(s).radius;
}

double wrap(double angle)
{
    double result = std::fmod(angle, tau);
    if (result < 0)
        result += tau;
    return result;
}

Point onCircle(Point center, double radius, double angle)
{
    return {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
}

Point start(const Segment &s)
{
    if (const auto *l = std::get_if<Line>(&s))
        return l->start;
    const auto &a = std::get<Arc>(s);
    return onCircle(a.center, a.radius, a.startAngle);
}

Point end(const Segment &s)
{
    if (const auto *l = std::get_if<Line>(&s))
        return l->end;
    const auto &a = std::get<Arc>(s);
    return onCircle(a.center, a.radius, a.startAngle + a.sweepAngle);
}

double arcDistance(const Arc &a, double angle)
{
    return a.sweepAngle > 0 ? wrap(angle - a.startAngle) : wrap(a.startAngle - angle);
}

bool onArc(const Arc &a, double angle, bool strict = false)
{
    double d = arcDistance(a, angle);
    if (tau - d <= angleTolerance)
        d = 0;
    return strict ? d > angleTolerance && d < std::abs(a.sweepAngle) - angleTolerance
                  : d <= std::abs(a.sweepAngle) + angleTolerance;
}

Circle circle(const Primitive &p)
{
    if (const auto *c = std::get_if<Circle>(&p))
        return *c;
    const auto &a = std::get<Arc>(p);
    return {a.center, a.radius};
}

bool containsPoint(const Primitive &p, Point point)
{
    if (const auto *a = std::get_if<Arc>(&p))
        return onArc(*a, std::atan2(point.y - a->center.y, point.x - a->center.x));
    return true;
}

std::vector<Primitive> primitives(const Contour &c)
{
    if (const auto *s = std::get_if<std::vector<Segment>>(&c)) {
        std::vector<Primitive> result;
        for (const auto &p : *s)
            std::visit([&](const auto &v) { result.emplace_back(v); }, p);
        return result;
    }
    return {std::get<Circle>(c)};
}

struct Intersection { std::vector<Point> points; bool overlap = false; };

Intersection lineLine(const Line &a, const Line &b)
{
    const Point da = sub(a.end, a.start), db = sub(b.end, b.start);
    const double la = length(da), lb = length(db);
    const Point ua = mul(da, 1 / la), ub = mul(db, 1 / lb), delta = sub(b.start, a.start);
    const double det = cross(ua, ub);
    const double tol = std::max(coordinateTolerance(a.start, b.start),
                                coordinateTolerance(a.end, b.end));
    if (det == 0) {
        if (std::abs(cross(delta, ua)) > tol)
            return {};
        const double x = dot(delta, ua), y = x + dot(db, ua);
        const double lo = std::max(0.0, std::min(x, y)), hi = std::min(la, std::max(x, y));
        if (hi < lo - tol)
            return {};
        if (hi > lo)
            return {{}, true};
        return {{add(a.start, mul(ua, std::clamp(lo, 0.0, la)))}, false};
    }
    const double x = cross(delta, ub) / det, y = cross(delta, ua) / det;
    if (!std::isfinite(x) || !std::isfinite(y))
        throw Error("geometry: numerically ambiguous LINE intersection");
    if (x < -tol || x > la + tol || y < -tol || y > lb + tol)
        return {};
    return {{add(a.start, mul(ua, std::clamp(x, 0.0, la)))}, false};
}

double chordHalfLength(double radius, double distance)
{
    const double q = std::abs(distance) / radius;
    if (q > 1)
        throw Error("geometry: numerically ambiguous circle intersection");
    return radius * std::sqrt((1 - q) * (1 + q));
}

Intersection lineCircle(const Line &l, const Primitive &p)
{
    const auto c = circle(p);
    const double size = length(sub(l.end, l.start));
    const Point unit = mul(sub(l.end, l.start), 1 / size), delta = sub(c.center, l.start);
    const double projection = dot(delta, unit), perpendicular = cross(delta, unit);
    const double tol = coordinateTolerance(l.start, add(c.center, {c.radius, c.radius}));
    if (std::abs(perpendicular) > c.radius + tol)
        return {};
    const double h = std::abs(perpendicular) > c.radius ? 0 : chordHalfLength(c.radius, perpendicular);
    Intersection result;
    for (double t : std::array{projection - h, projection + h}) {
        if (t >= -tol && t <= size + tol) {
            const Point point = add(l.start, mul(unit, std::clamp(t, 0.0, size)));
            if (containsPoint(p, point)
                && (result.points.empty() || point.x != result.points.back().x || point.y != result.points.back().y))
                result.points.push_back(point);
        }
    }
    return result;
}

Intersection circleCircle(const Primitive &a, const Primitive &b)
{
    const auto ca = circle(a), cb = circle(b);
    const Point delta = sub(cb.center, ca.center);
    const double d = length(delta);
    const double tol = coordinateTolerance(add(ca.center, {ca.radius, ca.radius}),
                                            add(cb.center, {cb.radius, cb.radius}));
    if (d <= tol && std::abs(ca.radius - cb.radius) <= tol) {
        if (std::holds_alternative<Circle>(a) || std::holds_alternative<Circle>(b))
            return {{}, true};
        const auto &aa = std::get<Arc>(a), &ab = std::get<Arc>(b);
        if (onArc(ab, aa.startAngle, true) || onArc(ab, aa.startAngle + aa.sweepAngle, true)
            || onArc(aa, ab.startAngle, true) || onArc(aa, ab.startAngle + ab.sweepAngle, true)
            || onArc(ab, aa.startAngle + aa.sweepAngle / 2, true))
            return {{}, true};
        Intersection result;
        for (const double angle : {aa.startAngle, aa.startAngle + aa.sweepAngle}) {
            if (onArc(ab, angle))
                result.points.push_back(onCircle(ca.center, ca.radius, angle));
        }
        return result;
    }
    if (d > ca.radius + cb.radius + tol || d < std::abs(ca.radius - cb.radius) - tol)
        return {};
    if (d <= tol)
        throw Error("geometry: numerically ambiguous concentric boundaries");
    const double scale = std::max({d, ca.radius, cb.radius});
    const double r1 = ca.radius / scale, r2 = cb.radius / scale, ds = d / scale;
    const double x = scale * ((r1 - r2) * (r1 + r2) + ds * ds) / (2 * ds);
    if (std::abs(x) > ca.radius + tol)
        return {};
    const double h = std::abs(x) > ca.radius ? 0 : chordHalfLength(ca.radius, x);
    const Point unit = mul(delta, 1 / d), mid = add(ca.center, mul(unit, x));
    const Point normal{-unit.y, unit.x};
    Intersection result;
    for (const Point point : {add(mid, mul(normal, h)), sub(mid, mul(normal, h))}) {
        if (containsPoint(a, point) && containsPoint(b, point)
            && (result.points.empty() || point.x != result.points.back().x || point.y != result.points.back().y))
            result.points.push_back(point);
    }
    return result;
}

Intersection intersections(const Primitive &a, const Primitive &b)
{
    if (const auto *l = std::get_if<Line>(&a)) {
        if (const auto *m = std::get_if<Line>(&b))
            return lineLine(*l, *m);
        return lineCircle(*l, b);
    }
    if (const auto *l = std::get_if<Line>(&b))
        return lineCircle(*l, a);
    return circleCircle(a, b);
}

Point uv(Point p, Direction direction)
{
    return direction == Direction::Horizontal ? p : Point{p.y, p.x};
}

std::vector<Interval> unite(std::vector<Interval> intervals)
{
    std::sort(intervals.begin(), intervals.end(), [](Interval a, Interval b) {
        return a.min < b.min || (a.min == b.min && a.max < b.max);
    });
    std::vector<Interval> result;
    for (const auto i : intervals) {
        if (!result.empty() && i.min <= result.back().max)
            result.back().max = std::max(result.back().max, i.max);
        else
            result.push_back(i);
    }
    return result;
}

// Subtract open intervals, retaining their boundary points.
std::vector<Interval> subtract(std::vector<Interval> source, const std::vector<Interval> &holes)
{
    for (const auto hole : holes) {
        if (hole.min == hole.max)
            continue;
        std::vector<Interval> next;
        for (const auto i : source) {
            if (i.max <= hole.min || i.min >= hole.max) {
                next.push_back(i);
            } else {
                if (i.min <= hole.min)
                    next.push_back({i.min, hole.min});
                if (i.max >= hole.max)
                    next.push_back({hole.max, i.max});
            }
        }
        source = std::move(next);
    }
    return source;
}

struct Slice
{
    std::vector<Interval> interior;
    std::vector<Interval> boundary;
};

std::vector<Point> junctions(const std::vector<Segment> &segments)
{
    std::vector<Point> result;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto &previous = segments[(i + segments.size() - 1) % segments.size()];
        // Resolve one numerical representation per structural junction; primitives remain unchanged.
        result.push_back(std::holds_alternative<Line>(segments[i]) ? start(segments[i])
                         : std::holds_alternative<Line>(previous) ? end(previous) : start(segments[i]));
    }
    return result;
}

Slice slice(const Contour &contour, Direction direction, double c)
{
    if (const auto *circle = std::get_if<Circle>(&contour)) {
        const Point center = uv(circle->center, direction);
        const double d = c - center.y;
        if (std::abs(d) > circle->radius)
            return {};
        const double h = chordHalfLength(circle->radius, d);
        if (h == 0)
            return {{}, {{center.x, center.x}}};
        if (center.x - h == center.x + h)
            throw Error("scan: positive chord is not representable", 5);
        return {{{center.x - h, center.x + h}},
                {{center.x - h, center.x - h}, {center.x + h, center.x + h}}};
    }
    const auto &segments = std::get<std::vector<Segment>>(contour);
    const auto vertices = junctions(segments);
    Slice result;
    std::vector<double> events;
    const auto branch = [&](Point a, Point b, const Arc *arc, double midAngle) {
        a = uv(a, direction);
        b = uv(b, direction);
        const double low = std::min(a.y, b.y), high = std::max(a.y, b.y);
        if (c < low || c > high)
            return;
        if (low == high) {
            if (arc)
                throw Error("scan: unresolved ARC transverse extent", 5);
            result.boundary.push_back({std::min(a.x, b.x), std::max(a.x, b.x)});
            return;
        }
        double u;
        if (c == a.y) {
            u = a.x;
        } else if (c == b.y) {
            u = b.x;
        } else if (!arc) {
            u = std::lerp(a.x, b.x, (c - a.y) / (b.y - a.y));
        } else {
            const Point center = uv(arc->center, direction);
            const double h = chordHalfLength(arc->radius, c - center.y);
            const double sign = direction == Direction::Horizontal ? std::cos(midAngle) : std::sin(midAngle);
            u = center.x + (sign > 0 ? h : -h);
        }
        if (!std::isfinite(u))
            throw Error("scan: nonfinite intersection", 5);
        result.boundary.push_back({u, u});
        // Half-open transverse ranges count a shared vertex once and a tangent zero times modulo two.
        if (c < high)
            events.push_back(u);
    };
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const Point first = vertices[i], last = vertices[(i + 1) % vertices.size()];
        if (std::holds_alternative<Line>(segments[i])) {
            branch(first, last, nullptr, 0);
        } else {
            const auto &a = std::get<Arc>(segments[i]);
            std::vector<std::pair<double, Point>> cuts{{0, first}, {1, last}};
            for (int k = 0; k < 4; ++k) {
                const double angle = k * pi / 2;
                const double d = arcDistance(a, angle);
                if (d > angleTolerance && d < std::abs(a.sweepAngle) - angleTolerance) {
                    const Point unit = k == 0 ? Point{1, 0} : k == 1 ? Point{0, 1}
                                     : k == 2 ? Point{-1, 0} : Point{0, -1};
                    cuts.emplace_back(d / std::abs(a.sweepAngle), add(a.center, mul(unit, a.radius)));
                }
            }
            std::sort(cuts.begin(), cuts.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
            for (std::size_t j = 1; j < cuts.size(); ++j) {
                const double mid = a.startAngle + a.sweepAngle * std::midpoint(cuts[j - 1].first, cuts[j].first);
                branch(cuts[j - 1].second, cuts[j].second, &a, mid);
            }
        }
    }
    std::sort(events.begin(), events.end());
    bool inside = false;
    double begin = 0;
    for (std::size_t i = 0; i < events.size();) {
        std::size_t j = i + 1;
        while (j < events.size() && events[j] == events[i])
            ++j;
        if ((j - i) % 2 != 0) {
            if (inside)
                result.interior.push_back({begin, events[i]});
            else
                begin = events[i];
            inside = !inside;
        }
        i = j;
    }
    if (inside)
        throw Error("scan: unbalanced contour crossings", 5);
    result.boundary = unite(std::move(result.boundary));
    return result;
}

bool strictlyInside(const Contour &contour, Point point)
{
    const auto s = slice(contour, Direction::Horizontal, point.y);
    for (const auto i : s.boundary)
        if (point.x >= i.min && point.x <= i.max)
            return false;
    for (const auto i : s.interior)
        if (point.x > i.min && point.x < i.max)
            return true;
    return false;
}

Point representative(const Contour &contour)
{
    if (const auto *c = std::get_if<Circle>(&contour))
        return {c->center.x + c->radius, c->center.y};
    return start(std::get<std::vector<Segment>>(contour).front());
}

void validateContour(const Contour &contour, const std::string &path)
{
    if (const auto *c = std::get_if<Circle>(&contour)) {
        if (!finite(c->center) || !std::isfinite(c->radius) || c->radius <= 0)
            throw Error(path + ": invalid CIRCLE");
    } else {
        const auto &segments = std::get<std::vector<Segment>>(contour);
        if (segments.empty())
            throw Error(path + ".segments: empty contour");
        if (segments.size() == 1)
            throw Error(path + ".segments: one LINE/ARC cannot form a closed contour");
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const std::string p = path + ".segments[" + std::to_string(i) + "]";
            if (const auto *l = std::get_if<Line>(&segments[i])) {
                if (!finite(l->start) || !finite(l->end) || !(length(sub(l->end, l->start)) > 0)
                    || !std::isfinite(length(sub(l->end, l->start))))
                    throw Error(p + ": zero or unrepresentable LINE length");
            } else {
                const auto &a = std::get<Arc>(segments[i]);
                if (!finite(a.center) || !std::isfinite(a.radius) || a.radius <= 0
                    || !std::isfinite(a.startAngle) || !std::isfinite(a.sweepAngle)
                    || a.sweepAngle == 0 || std::abs(a.sweepAngle) >= tau
                    || a.startAngle + a.sweepAngle == a.startAngle)
                    throw Error(p + ": invalid or numerically unresolved ARC");
            }
            if (!finite(start(segments[i])) || !finite(end(segments[i])))
                throw Error(p + ": nonfinite endpoint");
            const auto &next = segments[(i + 1) % segments.size()];
            if (!near(end(segments[i]), start(next), std::max(segmentScale(segments[i]), segmentScale(next))))
                throw Error(p + ": contour is not closed at next segment");
        }
        const auto all = primitives(contour);
        for (std::size_t i = 0; i < all.size(); ++i) {
            for (std::size_t j = i + 1; j < all.size(); ++j) {
                const auto hit = intersections(all[i], all[j]);
                const std::string p = path + ".segments[" + std::to_string(i) + "] / segments["
                                      + std::to_string(j) + "]";
                if (hit.overlap)
                    throw Error(p + ": overlapping primitives");
                for (const Point point : hit.points) {
                    const double localScale = std::max(segmentScale(segments[i]), segmentScale(segments[j]));
                    const bool next = j == i + 1 && near(point, end(segments[i]), localScale);
                    const bool closure = i == 0 && j + 1 == all.size() && near(point, start(segments[i]), localScale);
                    if (!next && !closure)
                        throw Error(p + ": self-intersection or self-touch");
                }
            }
        }
    }
    for (const auto &p : primitives(contour)) {
        if (!std::holds_alternative<Line>(p)) {
            const auto c = circle(p);
            if (!finite(add(c.center, {c.radius, c.radius}))
                || !finite(sub(c.center, {c.radius, c.radius})))
                throw Error(path + ": unrepresentable circle bounds");
        }
    }
}

void disjoint(const Contour &a, const Contour &b, const std::string &path)
{
    for (const auto &pa : primitives(a)) {
        for (const auto &pb : primitives(b)) {
            const auto hit = intersections(pa, pb);
            if (hit.overlap || !hit.points.empty())
                throw Error(path + ": independent contours intersect or touch");
        }
    }
}

} // namespace

void validateGeometry(const PartGeometry &geometry)
{
    validateContour(geometry.outer, "outerContour");
    for (std::size_t i = 0; i < geometry.inner.size(); ++i) {
        const std::string path = "innerContours[" + std::to_string(i) + "]";
        validateContour(geometry.inner[i], path);
        disjoint(geometry.outer, geometry.inner[i], path + " / outerContour");
        if (!strictlyInside(geometry.outer, representative(geometry.inner[i])))
            throw Error(path + ": inner contour is not strictly inside outerContour");
        for (std::size_t j = 0; j < i; ++j) {
            disjoint(geometry.inner[i], geometry.inner[j], path + " / innerContours[" + std::to_string(j) + "]");
            if (strictlyInside(geometry.inner[i], representative(geometry.inner[j]))
                || strictlyInside(geometry.inner[j], representative(geometry.inner[i])))
                throw Error(path + ": nested inner contours are unsupported");
        }
    }
    const auto b = geometryBounds(geometry);
    if (!finite(b.min) || !finite(b.max) || !finite(sub(b.max, b.min))
        || !(b.max.x > b.min.x) || !(b.max.y > b.min.y))
        throw Error("outerContour: degenerate or unrepresentable bounds");
}

Bounds geometryBounds(const PartGeometry &geometry)
{
    const double inf = std::numeric_limits<double>::infinity();
    Bounds b{{inf, inf}, {-inf, -inf}};
    const auto include = [&](Point p) {
        b.min.x = std::min(b.min.x, p.x); b.min.y = std::min(b.min.y, p.y);
        b.max.x = std::max(b.max.x, p.x); b.max.y = std::max(b.max.y, p.y);
    };
    for (const auto &p : primitives(geometry.outer)) {
        if (const auto *l = std::get_if<Line>(&p)) {
            include(l->start); include(l->end);
        } else {
            const auto c = circle(p);
            if (const auto *a = std::get_if<Arc>(&p)) {
                include(onCircle(a->center, a->radius, a->startAngle));
                include(onCircle(a->center, a->radius, a->startAngle + a->sweepAngle));
            }
            for (int k = 0; k < 4; ++k) {
                if (const auto *a = std::get_if<Arc>(&p); a && !onArc(*a, k * pi / 2))
                    continue;
                const Point unit = k == 0 ? Point{1, 0} : k == 1 ? Point{0, 1}
                                 : k == 2 ? Point{-1, 0} : Point{0, -1};
                include(add(c.center, mul(unit, c.radius)));
            }
        }
    }
    return b;
}

std::vector<Interval> materialIntervals(const PartGeometry &geometry,
                                      Direction direction, double coordinate)
{
    if (!std::isfinite(coordinate))
        throw Error("scan: nonfinite scan line", 5);
    auto outer = slice(geometry.outer, direction, coordinate);
    outer.interior.insert(outer.interior.end(), outer.boundary.begin(), outer.boundary.end());
    auto result = unite(std::move(outer.interior));
    for (const auto &inner : geometry.inner) {
        const auto hole = slice(inner, direction, coordinate);
        // Collinear boundary runs are part of the measured surface, not the hole's strict interior.
        auto strict = subtract(hole.interior, hole.boundary);
        result = subtract(std::move(result), strict);
    }
    return unite(std::move(result));
}

} // namespace synthetic
