#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

// Experimental inputs only. No production raster/storage contract is defined here.
namespace density {
using U64 = std::uint64_t;
inline constexpr U64 MiB = 1024 * 1024;
struct Failure : std::runtime_error {
    Failure(QString status, QString detail);
    QString status;
};
void require(bool condition, const QString &detail);
U64 add(U64 a, U64 b);
U64 multiply(U64 a, U64 b);
QString decimal(U64 n);
U64 integer(const QJsonObject &json, const char *key);

struct TileKey { U64 x = 0, y = 0; bool operator==(const TileKey &) const = default; };
struct Tile {
    TileKey key;
    std::uint32_t width = 0, height = 0;
    std::vector<U64> counts;
    bool operator==(const Tile &) const = default;
};
struct Spec {
    QString name;
    U64 width = 0, height = 0;
    std::uint32_t side = 0;
    U64 seed = 0;
    bool wide = false;
};
QStringList scenarioNames();
Spec makeSpec(const QString &name, std::uint32_t side, U64 gridSide, U64 seed);
QJsonObject toJson(const Spec &spec);
Spec specFromJson(const QJsonObject &json);
U64 tileCount(const Spec &spec);
TileKey keyAt(const Spec &spec, U64 ordinal);
U64 ordinalOf(const Spec &spec, TileKey key);
bool stored(const Spec &spec, TileKey key);
Tile generate(const Spec &spec, TileKey key);
U64 inputBytes(const Spec &spec);
QByteArray encode(const Tile &tile);
Tile decode(const QByteArray &bytes);
U64 sum(const Tile &tile);

// The same plans are passed to future storage readers. ROI is half-open in cells;
// a sink receives whole intersecting tiles, and can crop to ROI without rebinning.
enum class Access { Full, Tiles, Roi };
struct ReadPlan {
    Access access = Access::Full;
    std::vector<TileKey> tiles;
    U64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
QJsonObject toJson(const ReadPlan &plan);
std::vector<std::pair<QString, ReadPlan>> readPlans(const Spec &spec);
using TileSink = std::function<void(const Tile &)>;
void visit(const Spec &spec, const ReadPlan &plan, const std::function<void(TileKey)> &sink);
struct FileIo { U64 readBytes = 0, writeBytes = 0, reads = 0, writes = 0; };
QByteArray readFile(const QString &path, U64 maxBytes, FileIo &io);
void writeNew(const QString &path, const QByteArray &bytes, FileIo &io);
QString tilePath(const QString &directory, U64 ordinal);
void writeInputs(const QString &directory, const Spec &spec, FileIo &io,
                 const std::function<void()> &checkpoint);
void readInputs(const QString &directory, const Spec &spec, const ReadPlan &plan,
                const TileSink &sink, FileIo &io, const std::function<void()> &checkpoint);
// SHA-256 over canonical y/x ordered LE records, including dimensions and zero tiles.
QJsonObject fingerprint(const Spec &spec, const std::function<Tile(TileKey)> &source);
} // namespace density
