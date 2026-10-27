#pragma once
#include "oracle.h"
#include "measurement.h"
#include <QFile>

// 06.0C only. The coordinate policy and output stream are experimental.
namespace density::construction {
enum class Candidate { Sequential, Spatial };
struct Parameters {
    Grid grid;
    std::uint32_t tileSide = 256, groupTiles = 2;
    U64 eventCapacity = 65536, maxGroups = 1000000, maxOccupiedGroups = 1000000;
    U64 maxBoundaryChecks = 100000000;
    Limits limits{512 * MiB, 1024 * MiB, 1024 * MiB};
};
using Check = std::function<void()>;
struct Layout {
    U64 side, columns, rows, groups, cells;
    Spec spec;
};
Layout layout(const Parameters &p);
Plan plan(const Parameters &p, U64 points);
// Same rounded binary64 boundaries as the 06.0A interval oracle; binary search
// avoids subtraction/floor rounding across a boundary (including nextafter(0)).
TileKey cell(const Grid &grid, cadcontour::ModelPoint point);
void validateGrid(const Grid &grid, const Check &check);
cadcontour::cache::BoundingBox queryBox(const Parameters &p, U64 group);
template<class T> T take(cadcontour::cache::Result<T> result) {
    if (!result) {
        const auto &e = result.error();
        throw Failure(e.code == cadcontour::cache::ErrorCode::Cancelled ? "cancelled"
            : e.code == cadcontour::cache::ErrorCode::ResourceExhausted ? "resource_refused" : "cache_error",
            QString("Cache %1/%2/%3: %4").arg(int(e.code)).arg(int(e.phase)).arg(int(e.operation)).arg(e.detail));
    }
    return std::move(result.value());
}
// Borrowed tiles; completion is signalled only by a successful return. Sink must
// stage any output. All zero tiles are implicit. Group traversal is Y/X.
QJsonObject build(Candidate candidate, cadcontour::cache::CacheReader &reader,
                  const Parameters &p, const QString &newScratch,
                  const TileSink &sink, FileIo &io, const Check &check);

// Common uncompressed verification stream, using existing encode(Tile).
// One file, sparse in zero regions. Descriptors are explicitly budgeted.
class Output {
public:
    Output(const QString &path, const Spec &spec, U64 points, U64 budget, FileIo &io, U64 maxTiles = U64(-1));
    void append(const Tile &tile);
    void finish(U64 expectedPoints);
    QJsonObject verify(const std::vector<U64> &expected, const Check &check);
    double outputMs = 0;
private:
    struct Entry { TileKey key; U64 offset, bytes; };
    QFile file_;
    Spec spec_;
    std::vector<Entry> entries_;
    U64 total_ = 0, bytes_ = 0, budget_;
    FileIo &io_;
    bool finished_ = false;
};
} // namespace density::construction
