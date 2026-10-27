#pragma once
#include "dataset.h"
#include "cache/cache_types.h"
#include <QJsonArray>
#include <span>

namespace density {
struct Grid {
    cadcontour::cache::BoundingBox bounds;
    double cell = 1;
    U64 width = 0, height = 0;
};
Grid makeGrid(cadcontour::cache::BoundingBox bounds, double cell);
QJsonObject toJson(const Grid &grid);
Grid gridFromJson(const QJsonObject &json);
// Deliberately slow small-data reference: compare against every cell's intervals.
// It does not use floor(), integer division, tile addressing or a candidate kernel.
U64 oracleCell(const Grid &grid, cadcontour::ModelPoint point);
std::vector<U64> oracleCounts(const Grid &grid, std::span<const cadcontour::cache::Measurement> points);
struct RasterCase {
    QString name;
    Grid grid;
    std::vector<cadcontour::cache::Measurement> points;
    std::vector<U64> expectedCells; // manually specified per-record cell labels
};
std::vector<RasterCase> rasterCases();
QJsonArray countsJson(const std::vector<U64> &counts);
std::vector<U64> countsFromJson(const QJsonArray &json);
} // namespace density
