#include "measurement.h"
#include "cache/cache_reader.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <array>
#include <bit>
#include <map>
#include <vector>

using namespace cadcontour::cache;

namespace {
template<class T> T take(Result<T> result)
{
    if (!result)
        throw std::runtime_error(QStringLiteral("cache error %1/%2/%3: %4")
            .arg(int(result.error().code)).arg(int(result.error().phase))
            .arg(int(result.error().operation)).arg(result.error().detail).toStdString());
    return std::move(result.value());
}
QJsonObject statistics(const ReadStatistics &s)
{
    QJsonObject result;
    number(result, "indexPages", s.indexPages);
    number(result, "pointPages", s.pointPages);
    number(result, "readBytes", s.readBytes);
    number(result, "returnedRecords", s.returnedRecords);
    return result;
}
QString bits(double value) { return QString::number(std::bit_cast<std::uint64_t>(value), 16).rightJustified(16, u'0'); }
bool contains(const std::vector<BoundingBox> &boxes, cadcontour::ModelPoint point)
{
    for (const auto &box : boxes)
        if (point.x >= box.minX && point.x <= box.maxX && point.y >= box.minY && point.y <= box.maxY)
            return true;
    return false;
}
void hashRecord(QCryptographicHash &hash, const PointRecord &record)
{
    std::array<std::byte, format::recordSize> bytes{};
    take(encodeRecord(bytes, record));
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
}
}

QJsonObject validateCache(const QString &path, const QString &scratch, const QString &oraclePath)
{
    const auto start = std::chrono::steady_clock::now();
    QJsonObject report{{"cachePath", path}, {"sourceOpened", false}};
    auto reader = take(CacheReader::open(path, {1ULL << 20, 0, 1}));
    const auto &metadata = reader->metadata();
    report["cacheId"] = QUuid::fromRfc4122(QByteArrayView(
        reinterpret_cast<const char *>(metadata.id.bytes.data()), 16)).toString(QUuid::WithoutBraces);
    number(report, "pointCount", metadata.pointCount);
    number(report, "cacheBytes", reader->layout().fileSize);
    report["formatVersion"] = int(metadata.formatVersion);
    report["encoding"] = int(metadata.coordinateEncoding);
    report["indexHeight"] = int(reader->index().height);
    report["hasNonZeroZ"] = metadata.hasNonZeroZ;
    report["bounds"] = QJsonArray{metadata.bounds.minX, metadata.bounds.minY,
                                 metadata.bounds.maxX, metadata.bounds.maxY};
    require(QDir().mkpath(scratch), "cannot create validation scratch");
    const auto validationStart = std::chrono::steady_clock::now();
    report["strictValidation"] = statistics(take(reader->validate(
        {128ULL << 20, 8ULL << 30, 32}, scratch)));
    report["strictValidationMs"] = since(validationStart);

    std::map<RecordId, std::pair<QString, QString>> oracle;
    if (!oraclePath.isEmpty()) {
        QFile input(oraclePath);
        require(input.open(QIODevice::ReadOnly), "cannot open oracle");
        require(input.size() <= 1024 * 1024, "oracle must be bounded");
        const auto document = QJsonDocument::fromJson(input.readAll()).object();
        require(document["pointCount"].toString().toULongLong() == metadata.pointCount, "oracle count mismatch");
        const auto records = document["records"].toArray();
        require(!records.isEmpty() && records.size() <= 4096, "invalid oracle samples");
        for (const auto value : records) {
            const auto record = value.toObject();
            oracle.emplace(record["id"].toString().toULongLong(),
                std::pair(record["xBits"].toString(), record["yBits"].toString()));
        }
    }
    const std::vector<std::vector<BoundingBox>> queries{
        {{10, 10, 15, 15}},
        {{99, -1, 101, 1001}},
        {{0, 0, 25, 25}, {10, 10, 35, 35}, {0, 0, 25, 25}, {2200, 400, 2300, 600}}
    };
    // scanAll supplies independent membership. Compare ordered SHA-256 of full
    // ID/X/Y records, not weak sums/XOR or only counts.
    std::array<QCryptographicHash, 3> expected{QCryptographicHash(QCryptographicHash::Sha256),
        QCryptographicHash(QCryptographicHash::Sha256), QCryptographicHash(QCryptographicHash::Sha256)};
    std::array<std::uint64_t, 3> expectedCounts{};
    std::uint64_t count = 0, checked = 0;
    const auto scanStart = std::chrono::steady_clock::now();
    const auto scan = take(reader->scanAll([&](auto records) -> Status {
        for (const auto &record : records) {
            ++count;
            if (const auto found = oracle.find(record.id); found != oracle.end()) {
                require(bits(record.point.x) == found->second.first
                    && bits(record.point.y) == found->second.second, "binary64 oracle mismatch");
                ++checked;
            }
            for (std::size_t i = 0; i < queries.size(); ++i)
                if (contains(queries[i], record.point)) {
                    ++expectedCounts[i];
                    hashRecord(expected[i], record);
                }
        }
        return success();
    }));
    require(count == metadata.pointCount && checked == oracle.size(), "scan/oracle coverage mismatch");
    report["scanAll"] = statistics(scan);
    report["scanAllMs"] = since(scanStart);
    number(report, "binary64ControlRecords", checked);
    QJsonArray queryReports;
    for (std::size_t i = 0; i < queries.size(); ++i) {
        QCryptographicHash actual(QCryptographicHash::Sha256);
        std::uint64_t returned = 0;
        const auto sink = [&](auto records) -> Status {
            for (const auto &record : records) { ++returned; hashRecord(actual, record); }
            return success();
        };
        const auto queryStart = std::chrono::steady_clock::now();
        const auto stats = i < 2 ? take(reader->scanBox(queries[i].front(), sink))
                                 : take(reader->scanBoxes(queries[i], sink));
        const auto digest = actual.result();
        require(returned == expectedCounts[i] && digest == expected[i].result(), "spatial query mismatch");
        auto item = statistics(stats);
        item["ms"] = since(queryStart);
        item["sha256IdXy"] = QString::fromLatin1(digest.toHex());
        queryReports.append(item);
    }
    report["spatialQueries"] = queryReports;
    report["totalMs"] = since(start);
    report["status"] = "PASS";
    return report;
}
