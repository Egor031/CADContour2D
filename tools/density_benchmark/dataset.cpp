#include "dataset.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QtEndian>

#include <algorithm>
#include <limits>

namespace density {
Failure::Failure(QString state, QString detail)
    : std::runtime_error(detail.toStdString()), status(std::move(state)) {}
void require(bool c, const QString &message) { if (!c) throw Failure("invalid_input", message); }
U64 add(U64 a, U64 b) {
    if (b > std::numeric_limits<U64>::max() - a) throw Failure("resource_refused", "integer addition overflow");
    return a + b;
}
U64 multiply(U64 a, U64 b) {
    if (b && a > std::numeric_limits<U64>::max() / b) throw Failure("resource_refused", "integer multiplication overflow");
    return a * b;
}
QString decimal(U64 n) { return QString::number(n); }
U64 integer(const QJsonObject &j, const char *key) {
    bool ok = false;
    auto value = j[key].toString().toULongLong(&ok);
    require(ok, QString("invalid unsigned decimal field: %1").arg(key));
    return value;
}
namespace {
U64 columns(const Spec &s) { return 1 + (s.width - 1) / s.side; }
U64 rows(const Spec &s) { return 1 + (s.height - 1) / s.side; }
bool huge(const Spec &s) { return s.name == "huge-sparse"; }
U64 randomWord(U64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
void validate(const Spec &s) {
    require(scenarioNames().contains(s.name) && s.side >= 4 && s.side <= 1024
        && s.width && s.height && s.wide == (s.name == "u64"), "invalid dataset specification");
    multiply(s.width, s.height);
    if (huge(s)) require(columns(s) > 1 && rows(s) > 1, "huge-sparse needs distinct corners");
}
template<class T> void put(QByteArray &b, T value) {
    char bytes[sizeof(T)]; qToLittleEndian<T>(value, bytes); b.append(bytes, sizeof(T));
}
template<class T> T get(const QByteArray &b, qsizetype offset) {
    return qFromLittleEndian<T>(b.constData() + offset);
}
}
QStringList scenarioNames() {
    return {"empty", "uniform", "dense", "sparse", "horizontal", "vertical", "clustered",
            "overlap", "u32", "u64", "entropy", "huge-sparse"};
}
Spec makeSpec(const QString &name, std::uint32_t side, U64 gridSide, U64 seed) {
    require(side >= 4 && side <= 1024 && gridSide >= 2, "tile-size must be 4..1024 and grid-side >= 2");
    auto extent = multiply(side, gridSide);
    Spec s{name, extent - 1, extent - 3, side, seed, name == "u64"};
    if (huge(s)) { s.width = 1000000003; s.height = 1000000001; }
    validate(s); return s;
}
QJsonObject toJson(const Spec &s) {
    return {{"name", s.name}, {"width", decimal(s.width)}, {"height", decimal(s.height)},
        {"tileSide", decimal(s.side)}, {"seed", decimal(s.seed)}, {"countBits", s.wide ? 64 : 32},
        {"inputSchema", "density-input-06.0A-v1"}};
}
Spec specFromJson(const QJsonObject &j) {
    require(j["inputSchema"] == "density-input-06.0A-v1", "unsupported experimental input schema");
    auto side = integer(j, "tileSide");
    require(side <= 1024 && (j["countBits"] == 32 || j["countBits"] == 64), "invalid input encoding");
    Spec s{j["name"].toString(), integer(j, "width"), integer(j, "height"),
        static_cast<std::uint32_t>(side), integer(j, "seed"), j["countBits"] == 64};
    validate(s); return s;
}
U64 tileCount(const Spec &s) { validate(s); return huge(s) ? 4 : multiply(columns(s), rows(s)); }
TileKey keyAt(const Spec &s, U64 ordinal) {
    require(ordinal < tileCount(s), "tile ordinal outside dataset");
    if (huge(s)) return {(ordinal & 1) ? columns(s) - 1 : 0, (ordinal & 2) ? rows(s) - 1 : 0};
    return {ordinal % columns(s), ordinal / columns(s)};
}
U64 ordinalOf(const Spec &s, TileKey key) {
    validate(s);
    require(key.x < columns(s) && key.y < rows(s), "tile outside raster");
    if (huge(s)) {
        require((key.x == 0 || key.x == columns(s) - 1) && (key.y == 0 || key.y == rows(s) - 1),
                "implicit zero tile has no input file");
        return (key.x ? 1 : 0) + (key.y ? 2 : 0);
    }
    return add(multiply(key.y, columns(s)), key.x);
}
bool stored(const Spec &s, TileKey key) {
    validate(s);
    require(key.x < columns(s) && key.y < rows(s), "tile outside raster");
    return !huge(s) || ((key.x == 0 || key.x == columns(s)-1) && (key.y == 0 || key.y == rows(s)-1));
}
Tile generate(const Spec &s, TileKey key) {
    const bool explicitTile = stored(s, key);
    const U64 startX = multiply(key.x, s.side), startY = multiply(key.y, s.side);
    Tile t{key, static_cast<std::uint32_t>(std::min<U64>(s.side, s.width - startX)),
        static_cast<std::uint32_t>(std::min<U64>(s.side, s.height - startY)), {}};
    t.counts.reserve(static_cast<std::size_t>(t.width) * t.height);
    for (U64 y = startY; y < startY + t.height; ++y) {
        for (U64 x = startX; x < startX + t.width; ++x) {
            const U64 noise = randomWord(s.seed ^ randomWord(x) ^ randomWord(y + 123));
            U64 n = 0;
            if (s.name == "uniform") n = 1;
            else if (s.name == "dense") n = 4096;
            else if (s.name == "sparse" || (huge(s) && explicitTile)) n = noise % 31 == 0 ? 7 : 0;
            else if (s.name == "horizontal") n = y % 4 == 0 ? 11 : 0;
            else if (s.name == "vertical") n = x % 4 == 0 ? 13 : 0;
            else if (s.name == "clustered") n = x / s.side == 1 && y / s.side == 1 ? 17 : 0;
            else if (s.name == "overlap") n = (x % 3 == 0 ? 5 : 0) + (y % 3 == 0 ? 7 : 0);
            else if (s.name == "u32") n = x % s.side == 0 && y % s.side == 0 ? 0xffffffffULL : 0;
            else if (s.name == "u64") n = x % s.side == 0 && y % s.side == 0 ? 0x100000001ULL : 0;
            else if (s.name == "entropy") n = noise & 0xffffffffULL;
            t.counts.push_back(n);
        }
    }
    return t;
}
U64 inputBytes(const Spec &s) {
    validate(s);
    // Only the four corner tiles are explicit for huge-sparse; all others are zero.
    if (huge(s)) {
        U64 bytes = 0;
        for (U64 i = 0; i < 4; ++i) {
            auto k = keyAt(s, i);
            auto w = std::min<U64>(s.side, s.width - k.x * s.side);
            auto h = std::min<U64>(s.side, s.height - k.y * s.side);
            bytes = add(bytes, add(32, multiply(multiply(w, h), 8)));
        }
        return bytes;
    }
    return add(multiply(tileCount(s), 32), multiply(multiply(s.width, s.height), 8));
}
QByteArray encode(const Tile &t) {
    require(t.width && t.height && t.width <= 1024 && t.height <= 1024
        && t.counts.size() == U64(t.width) * t.height, "invalid count tile");
    QByteArray b("D06AIN01", 8);
    put(b, t.key.x); put(b, t.key.y); put(b, t.width); put(b, t.height);
    for (auto n : t.counts) put(b, n);
    return b;
}
Tile decode(const QByteArray &b) {
    require(b.size() >= 32 && b.first(8) == "D06AIN01", "invalid count input header");
    Tile t{{get<U64>(b, 8), get<U64>(b, 16)}, get<std::uint32_t>(b, 24), get<std::uint32_t>(b, 28), {}};
    require(t.width && t.height && t.width <= 1024 && t.height <= 1024
        && U64(b.size()) == 32 + U64(t.width) * t.height * 8, "invalid count input length");
    for (qsizetype i = 32; i < b.size(); i += 8) t.counts.push_back(get<U64>(b, i));
    return t;
}
U64 sum(const Tile &t) { U64 total = 0; for (auto n : t.counts) total = add(total, n); return total; }
QJsonObject toJson(const ReadPlan &p) {
    QJsonObject j{{"access", p.access == Access::Full ? "full" : p.access == Access::Tiles ? "tiles" : "roi"}};
    if (p.access == Access::Tiles) {
        QJsonArray keys;
        for (auto k : p.tiles) keys.append(QJsonObject{{"x",decimal(k.x)},{"y",decimal(k.y)}});
        j["tiles"] = keys;
    }
    if (p.access == Access::Roi) j["halfOpenCellBounds"] = QJsonArray{decimal(p.x0),decimal(p.y0),decimal(p.x1),decimal(p.y1)};
    return j;
}
std::vector<std::pair<QString, ReadPlan>> readPlans(const Spec &s) {
    const auto first = keyAt(s,0), last = keyAt(s,tileCount(s)-1);
    std::vector<std::pair<QString, ReadPlan>> plans{
        {"full", {}}, {"tiles", {Access::Tiles, {last, first}, 0,0,0,0}},
        {"roi", {Access::Roi, {}, 0,0,U64(s.side)+1,U64(s.side)+1}}};
    if (huge(s)) {
        plans.push_back({"implicit-zero-tile",{Access::Tiles,{{1,1}},0,0,0,0}});
        plans.push_back({"empty-roi",{Access::Roi,{},s.side,s.side,U64(s.side)*2,U64(s.side)*2}});
    }
    return plans;
}
void visit(const Spec &s, const ReadPlan &p, const std::function<void(TileKey)> &sink) {
    if (p.access == Access::Tiles) { for (auto k : p.tiles) { stored(s, k); sink(k); } return; }
    if (p.access == Access::Roi) require(p.x0 < p.x1 && p.y0 < p.y1
        && p.x1 <= s.width && p.y1 <= s.height, "invalid half-open raster ROI");
    // Work is proportional to stored input tiles, including for a sparse logical raster.
    for (U64 i = 0; i < tileCount(s); ++i) {
        auto k = keyAt(s, i);
        const auto x = multiply(k.x,s.side), y = multiply(k.y,s.side);
        const auto endX = add(x,std::min<U64>(s.side,s.width-x));
        const auto endY = add(y,std::min<U64>(s.side,s.height-y));
        if (p.access == Access::Full || (x < p.x1 && y < p.y1 && endX > p.x0 && endY > p.y0)) sink(k);
    }
}
QByteArray readFile(const QString &path, U64 maxBytes, FileIo &io) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) throw Failure("io_error", f.errorString());
    require(f.size() >= 0 && U64(f.size()) <= maxBytes, "input file exceeds expected bound");
    QByteArray bytes(f.size(), Qt::Uninitialized);
    qint64 offset = 0;
    while (offset < bytes.size()) {
        auto n = f.read(bytes.data() + offset, bytes.size() - offset); ++io.reads;
        if (n <= 0) throw Failure("io_error", "short input read");
        io.readBytes = add(io.readBytes, U64(n)); offset += n;
    }
    return bytes;
}
void writeNew(const QString &path, const QByteArray &bytes, FileIo &io) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered)) throw Failure("io_error", f.errorString());
    qint64 offset = 0;
    while (offset < bytes.size()) {
        auto n = f.write(bytes.data() + offset, bytes.size() - offset); ++io.writes;
        if (n <= 0) throw Failure("io_error", "short output write");
        io.writeBytes = add(io.writeBytes, U64(n)); offset += n;
    }
    if (!f.flush()) throw Failure("io_error", f.errorString());
}
QString tilePath(const QString &dir, U64 ordinal) { return QDir(dir).filePath(decimal(ordinal) + ".counts"); }
void writeInputs(const QString &dir, const Spec &s, FileIo &io, const std::function<void()> &check) {
    require(!QFileInfo::exists(dir) && QDir().mkdir(dir), "input directory must be new");
    writeNew(QDir(dir).filePath("manifest.json"), QJsonDocument(toJson(s)).toJson(), io);
    for (U64 i = 0; i < tileCount(s); ++i) { check(); writeNew(tilePath(dir, i), encode(generate(s, keyAt(s, i))), io); }
}
void readInputs(const QString &dir, const Spec &s, const ReadPlan &p, const TileSink &sink,
                FileIo &io, const std::function<void()> &check) {
    visit(s, p, [&](TileKey key) {
        check();
        if (!stored(s,key)) { sink(generate(s,key)); return; }
        auto t = decode(readFile(tilePath(dir, ordinalOf(s, key)), 32 + U64(s.side) * s.side * 8, io));
        require(t.key == key && t.width == std::min<U64>(s.side, s.width - key.x * s.side)
            && t.height == std::min<U64>(s.side, s.height - key.y * s.side), "input tile does not match manifest");
        if (!s.wide) for (auto n : t.counts) require(n <= 0xffffffffULL, "uint32 input overflow");
        sink(t);
    });
}
QJsonObject fingerprint(const Spec &s, const std::function<Tile(TileKey)> &source) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QJsonDocument(toJson(s)).toJson(QJsonDocument::Compact));
    U64 total = 0, maximum = 0, nonzero = 0;
    for (U64 i = 0; i < tileCount(s); ++i) {
        auto t = source(keyAt(s, i));
        hash.addData(encode(t)); total = add(total, sum(t));
        for (auto n : t.counts) { maximum = std::max(maximum, n); nonzero += n != 0; }
    }
    return {{"sha256", QString::fromLatin1(hash.result().toHex())}, {"sum", decimal(total)},
        {"maximum", decimal(maximum)}, {"nonzeroCells", decimal(nonzero)}, {"storedTiles", decimal(tileCount(s))}};
}
} // namespace density
