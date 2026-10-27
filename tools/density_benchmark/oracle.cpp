#include "oracle.h"
#include <QJsonArray>

#include <cmath>

namespace density {
Grid makeGrid(cadcontour::cache::BoundingBox b, double cell) {
    require(cadcontour::cache::validBounds(b) && std::isfinite(cell) && cell > 0, "invalid raster bounds/cell");
    auto dimension = [cell](double lo, double hi) -> U64 {
        const auto n = std::max(1., std::ceil((hi - lo) / cell));
        require(std::isfinite(n) && n < std::ldexp(1., 64), "unrepresentable raster dimension");
        return static_cast<U64>(n);
    };
    Grid grid{b, cell, dimension(b.minX, b.maxX), dimension(b.minY, b.maxY)};
    multiply(grid.width, grid.height);
    return grid;
}
QJsonObject toJson(const Grid &g) {
    return {{"bounds", QJsonArray{g.bounds.minX, g.bounds.minY, g.bounds.maxX, g.bounds.maxY}},
        {"cellMm", g.cell}, {"width", decimal(g.width)}, {"height", decimal(g.height)},
        {"convention", "experimental-min-origin-y-up-half-open-clamped-max-v1"}};
}
Grid gridFromJson(const QJsonObject &j) {
    require(j["convention"] == "experimental-min-origin-y-up-half-open-clamped-max-v1", "unknown raster convention");
    auto b = j["bounds"].toArray();
    require(b.size() == 4, "invalid raster bounds array");
    for (const auto &v : b) require(v.isDouble(), "non-numeric raster bound");
    auto g = makeGrid({b[0].toDouble(), b[1].toDouble(), b[2].toDouble(), b[3].toDouble()}, j["cellMm"].toDouble());
    require(g.width == integer(j, "width") && g.height == integer(j, "height"), "raster dimensions mismatch");
    return g;
}
U64 oracleCell(const Grid &g, cadcontour::ModelPoint p) {
    require(multiply(g.width, g.height) <= 4096, "oracle is limited to 4096 cells");
    require(cadcontour::cache::validPoint(p) && p.x >= g.bounds.minX && p.x <= g.bounds.maxX
        && p.y >= g.bounds.minY && p.y <= g.bounds.maxY, "point outside raster bounds");
    U64 found = 0, label = 0;
    for (U64 y = 0; y < g.height; ++y) {
        const double loY = g.bounds.minY + double(y) * g.cell;
        const double hiY = g.bounds.minY + double(y + 1) * g.cell;
        const bool inY = p.y >= loY && (y + 1 == g.height ? p.y <= g.bounds.maxY : p.y < hiY);
        for (U64 x = 0; x < g.width; ++x) {
            const double loX = g.bounds.minX + double(x) * g.cell;
            const double hiX = g.bounds.minX + double(x + 1) * g.cell;
            const bool inX = p.x >= loX && (x + 1 == g.width ? p.x <= g.bounds.maxX : p.x < hiX);
            if (inX && inY) { ++found; label = y * g.width + x; }
        }
    }
    require(found == 1, "oracle requires exactly one cell per point (unrepresentable boundaries?)");
    return label;
}
std::vector<U64> oracleCounts(const Grid &g, std::span<const cadcontour::cache::Measurement> points) {
    require(multiply(g.width, g.height) <= 4096, "oracle raster too large");
    std::vector<U64> counts(static_cast<std::size_t>(g.width * g.height));
    for (auto p : points) { auto &n = counts[oracleCell(g, {p.x, p.y})]; n = add(n, 1); }
    return counts;
}
std::vector<RasterCase> rasterCases() {
    const auto before0 = std::nextafter(0., -1.);
    const auto after0 = std::nextafter(0., 1.);
    return {
        {"boundaries", makeGrid({-2, -1, 1, 1}, 1),
            {{-2,-1,0}, {-1,-1,0}, {0,-1,0}, {1,-1,0}, {-2,0,0}, {-1,0,0},
             {0,0,0}, {1,1,0}, {before0,0,0}, {after0,0,0}, {-0.,-0.,0}, {0,0,0}, {0,0,0}},
            {0,1,2,2,3,4,5,5,4,5,5,5,5}},
        {"degenerate-point", makeGrid({-3,2,-3,2}, .5), {{-3,2,0},{-3,2,0}}, {0,0}},
        {"degenerate-line", makeGrid({-1,-2,-1,1}, 1),
            {{-1,-2,0},{-1,-1,0},{-1,0,0},{-1,1,0}}, {0,1,2,2}},
        {"partial-cell", makeGrid({-.5,-.5,2,1}, 1),
            {{-.5,-.5,0},{.5,.5,0},{1.5,.5,0},{2,1,0}}, {0,4,5,5}},
        {"empty-points", makeGrid({0,0,0,0}, 1), {}, {}}
    };
}
QJsonArray countsJson(const std::vector<U64> &counts) {
    QJsonArray a; for (auto n : counts) a.append(decimal(n)); return a;
}
std::vector<U64> countsFromJson(const QJsonArray &a) {
    require(a.size() <= 4096, "oracle counts array too large");
    std::vector<U64> counts;
    for (const auto &v : a) counts.push_back(integer({{"n", v}}, "n"));
    return counts;
}
} // namespace density
