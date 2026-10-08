#include "generator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

namespace synthetic {
namespace {

QByteArray read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error((path + ": " + file.errorString()).toStdString(), 4);
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        throw Error((path + ": " + file.errorString()).toStdString(), 4);
    return bytes;
}

QByteArray hash(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

struct Coordinate
{
    std::array<char, 336> text;
    std::size_t size;
    double rounded;
};

Coordinate coordinate(double value)
{
    if (!std::isfinite(value))
        throw Error("output: nonfinite coordinate", 5);
    Coordinate result{};
    const auto converted = std::to_chars(result.text.data(), result.text.data() + result.text.size(),
                                         value, std::chars_format::fixed, 6);
    if (converted.ec != std::errc{})
        throw Error("output: coordinate formatting failed", 5);
    result.size = static_cast<std::size_t>(converted.ptr - result.text.data());
    if (std::string_view(result.text.data(), result.size) == "-0.000000") {
        std::copy_n("0.000000", 8, result.text.data());
        result.size = 8;
    }
    const auto parsed = std::from_chars(result.text.data(), result.text.data() + result.size, result.rounded);
    if (parsed.ec != std::errc{} || !std::isfinite(result.rounded))
        throw Error("output: rounded coordinate is not representable", 5);
    return result;
}

QString reference(const QString &path, const QDir &directory)
{
    return QDir::fromNativeSeparators(directory.relativeFilePath(QFileInfo(path).absoluteFilePath()));
}

void close(QTemporaryFile &file)
{
    if (!file.flush())
        throw Error((file.fileName() + ": " + file.errorString()).toStdString(), 4);
    file.close();
    if (file.error() != QFileDevice::NoError)
        throw Error((file.fileName() + ": " + file.errorString()).toStdString(), 4);
}

class Writer
{
public:
    explicit Writer(QIODevice &file) : file(file), sha(QCryptographicHash::Sha256)
    {
        buffer.reserve(1024 * 1024);
    }

    void point(Point p)
    {
        const auto x = coordinate(p.x), y = coordinate(p.y);
        if (stats.pointCount == std::numeric_limits<std::int64_t>::max())
            throw Error("output: point count overflow", 5);
        buffer.append(x.text.data(), static_cast<qsizetype>(x.size));
        buffer.append(' ');
        buffer.append(y.text.data(), static_cast<qsizetype>(y.size));
        buffer.append(" 0\n");
        const Point rounded{x.rounded, y.rounded};
        if (stats.pointCount == 0) {
            stats.bounds = {rounded, rounded};
        } else {
            stats.bounds.min.x = std::min(stats.bounds.min.x, rounded.x);
            stats.bounds.min.y = std::min(stats.bounds.min.y, rounded.y);
            stats.bounds.max.x = std::max(stats.bounds.max.x, rounded.x);
            stats.bounds.max.y = std::max(stats.bounds.max.y, rounded.y);
        }
        ++stats.pointCount;
        if (buffer.size() >= 1024 * 1024)
            flush();
    }

    DatasetStats finish()
    {
        flush();
        stats.sha256 = sha.result().toHex();
        return stats;
    }

private:
    void flush()
    {
        if (file.write(buffer) != buffer.size())
            throw Error(("output: write failed: " + file.errorString()).toStdString(), 4);
        sha.addData(buffer);
        buffer.clear();
    }

    QIODevice &file;
    QCryptographicHash sha;
    QByteArray buffer;
    DatasetStats stats;
};

bool exists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

} // namespace

DatasetStats writeDataset(const QString &geometryPath, const QString &scanPath,
                          const QString &outputPath)
{
    const QString output = QFileInfo(outputPath).absoluteFilePath();
    const QString manifest = output + ".manifest.json";
    const QString extension = QFileInfo(output).suffix();
    if (extension != "xyz" && extension != "asc")
        throw Error("output: expected .xyz or .asc extension", 2);
    for (const auto &target : {output, manifest}) {
        if (exists(target))
            throw Error((target + ": target already exists; refusing overwrite").toStdString(), 4);
    }
    const auto geometryBytes = read(geometryPath), scanBytes = read(scanPath);
    PartGeometry geometry;
    ScanScenario scan;
    try {
        geometry = parseGeometry(geometryBytes);
    } catch (const Error &e) {
        throw Error((geometryPath + ": " + QString::fromUtf8(e.what())).toStdString(), e.exitCode);
    }
    try {
        scan = parseScan(scanBytes);
    } catch (const Error &e) {
        throw Error((scanPath + ": " + QString::fromUtf8(e.what())).toStdString(), e.exitCode);
    }
    QTemporaryFile points(output + ".XXXXXX.tmp"), metadata(manifest + ".XXXXXX.tmp");
    if (!points.open() || !metadata.open())
        throw Error("output: cannot create temporary files next to target", 4);
    Writer writer(points);
    const auto bounds = geometryBounds(geometry);
    const bool horizontal = scan.direction == Direction::Horizontal;
    const double origin = horizontal ? bounds.min.y : bounds.min.x;
    const double limit = horizontal ? bounds.max.y : bounds.max.x;
    // Check representable intervals/gaps per line before quantizing its points.
    double checkedLine = std::numeric_limits<double>::quiet_NaN();
    generate(geometry, scan, [&](Point p) {
        const double v = horizontal ? p.y : p.x;
        if (v != checkedLine) {
            const auto intervals = materialIntervals(geometry, scan.direction, v);
            std::optional<double> previous;
            for (const auto i : intervals) {
                const double a = coordinate(i.min).rounded, b = coordinate(i.max).rounded;
                if (i.max > i.min && !(b > a))
                    throw Error("output: six-decimal precision collapses a positive interval", 5);
                if (previous && !(a > *previous))
                    throw Error("output: six-decimal precision collapses a material gap", 5);
                previous = b;
            }
            if (v < origin || v > limit)
                throw Error("output: scan line outside geometry bounds", 5);
            checkedLine = v;
        }
        writer.point(p);
    });
    const auto stats = writer.finish();
    close(points);
    const QDir directory = QFileInfo(output).absoluteDir();
    QJsonValue bbox = QJsonValue::Null;
    if (stats.pointCount != 0) {
        bbox = QJsonObject{{"min", QJsonArray{stats.bounds.min.x, stats.bounds.min.y, 0}},
                           {"max", QJsonArray{stats.bounds.max.x, stats.bounds.max.y, 0}}};
    }
    const QJsonObject doc{
        {"manifestVersion", 1}, {"generatorVersion", generatorVersion}, {"units", "mm"},
        {"geometry", QJsonObject{{"path", reference(geometryPath, directory)},
                                  {"sha256", QString::fromLatin1(hash(geometryBytes))}}},
        {"scan", QJsonObject{{"path", reference(scanPath, directory)},
                              {"sha256", QString::fromLatin1(hash(scanBytes))}}},
        {"seed", static_cast<qint64>(scan.seed)},
        {"direction", horizontal ? "Horizontal" : "Vertical"},
        {"pointStep", scan.pointStep}, {"lineStep", scan.lineStep}, {"maxLineLength", scan.maxLineLength},
        {"output", reference(output, directory)}, {"outputFormat", extension},
        {"outputSha256", QString::fromLatin1(stats.sha256)},
        {"pointCount", static_cast<qint64>(stats.pointCount)}, {"boundingBox", bbox}
    };
    const auto manifestBytes = QJsonDocument(doc).toJson(QJsonDocument::Indented);
    if (metadata.write(manifestBytes) != manifestBytes.size())
        throw Error("manifest: write failed", 4);
    close(metadata);
    if (!metadata.rename(manifest))
        throw Error(("manifest: publication failed: " + metadata.errorString()).toStdString(), 4);
    if (!points.rename(output)) {
        const bool removed = metadata.remove();
        throw Error(removed ? "output: publication failed; manifest rolled back"
                            : "output: publication failed; cannot remove published manifest", 4);
    }
    points.setAutoRemove(false);
    metadata.setAutoRemove(false);
    return stats;
}

} // namespace synthetic
