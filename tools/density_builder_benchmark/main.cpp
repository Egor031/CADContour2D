#include "density/density_builder.h"
#include "cache/cache_writer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>

using namespace cadcontour;
using namespace cadcontour::density;
namespace {
constexpr U64 MiB = 1024 * 1024;
QString n(U64 v) { return QString::number(v); }
double ms(U64 ns) { return double(ns) / 1e6; }
void require(bool ok, const char *detail) { if (!ok) throw std::runtime_error(detail); }
template<class T> T take(cache::Result<T> result)
{
    if (!result) throw std::runtime_error(result.error().detail.toStdString());
    return std::move(result.value());
}
template<class T> T take(Result<T> result)
{
    if (!result) throw std::runtime_error(result.error().detail.toStdString());
    return std::move(result.value());
}
U64 ticks(FILETIME t) { return (U64(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
struct Sample {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    IO_COUNTERS io{};
    U64 cpuTicks = 0;
};
Sample sample()
{
    Sample s; s.memory.cb = sizeof(s.memory);
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&s.memory), sizeof(s.memory)), "GetProcessMemoryInfo failed");
    require(GetProcessIoCounters(GetCurrentProcess(), &s.io), "GetProcessIoCounters failed");
    require(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user), "GetProcessTimes failed");
    s.cpuTicks = ticks(kernel) + ticks(user); return s;
}
QString git(QStringList args)
{
    QProcess p; p.setWorkingDirectory(DENSITY_BUILDER_SOURCE_DIR); p.start("git", args);
    if (!p.waitForFinished(10000) || p.exitCode()) return QStringLiteral("unavailable");
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}
QJsonObject environment(const QString &parent)
{
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    require(GlobalMemoryStatusEx(&memory), "GlobalMemoryStatusEx failed");
    QStorageInfo storage(parent);
    QFile executable(QCoreApplication::applicationFilePath());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(executable.open(QIODevice::ReadOnly) && hash.addData(&executable), "executable hash failed");
    return {{"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"compiler", DENSITY_BUILDER_COMPILER}, {"msvcFullVersion", n(_MSC_FULL_VER)},
        {"configuration", DENSITY_BUILDER_CONFIGURATION}, {"qt", qVersion()},
        {"architecture", QSysInfo::buildCpuArchitecture()}, {"os", QSysInfo::prettyProductName()},
        {"cpu", qEnvironmentVariable("PROCESSOR_IDENTIFIER")}, {"physicalRamBytes", n(memory.ullTotalPhys)},
        {"scratchParent", QFileInfo(parent).absoluteFilePath()}, {"volume", storage.rootPath()},
        {"fileSystem", QString::fromUtf8(storage.fileSystemType())}, {"device", QString::fromUtf8(storage.device())},
        {"mediaModel", "not detected by harness; see accompanying environment evidence"},
        {"head", git({"rev-parse", "HEAD"})}, {"gitStatus", git({"status", "--porcelain=v1"})},
        {"executableSha256", QString::fromLatin1(hash.result().toHex())},
        {"cacheCondition", "uncontrolled OS cache; source just prepared/reused; never claimed cold"},
        {"memoryDefinition", "working-set snapshots before/after build; Windows peak is process-lifetime and includes preparation/oracle"},
        {"processIoDefinition", "GetProcessIoCounters build-only logical transfer/operation deltas; not physical disk I/O"},
        {"cpuDefinition", "GetProcessTimes kernel+user delta in 100ns ticks; build-only"}};
}
struct Profile { QString name; U64 groups, extent; bool compact = false; };
// The oracle starts with known rational cell positions. It never calls Mapping,
// production group addressing, or production accumulation to compute expectations.
struct Expected {
    U64 tileY = 0, tileX = 0, local = 0, count = 1;
    auto key() const { return std::tie(tileY, tileX, local); }
};
struct Prepared {
    std::optional<cache::PendingCache> cache;
    ImportedDataset source;
    std::vector<Expected> expected;
    U64 width = 0, height = 0, expectedTiles = 0;
    double preparationMs = 0;
};
U64 mix(U64 value)
{
    value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27; value *= 0x94d049bb133111ebULL; return value ^ (value >> 31);
}
Prepared prepare(const Profile &profile, U64 count, const QString &directory)
{
    Prepared prepared; prepared.width = prepared.height = profile.extent;
    require(count >= profile.groups + 2, "point count too small for profile");
    require(count <= std::numeric_limits<std::size_t>::max() / sizeof(Expected), "oracle size unrepresentable");
    prepared.expected.reserve(static_cast<std::size_t>(count));
    QElapsedTimer timer; timer.start();
    auto writer = take(cache::CacheWriter::create(directory + "/source.pc2d", {64 * MiB, 1024 * MiB, 16}, directory));
    std::vector<cache::Measurement> batch; batch.reserve(4096);
    auto appendSample = [&](U64 x, U64 y, bool maximum = false) {
        // Normal sample lies strictly inside its prescribed cell. The two anchors
        // set exact bounds [.25, extent+.25]; the maximum belongs to extent-1.
        batch.push_back({double(x) + (maximum ? 1.25 : .5), double(y) + (maximum ? 1.25 : .5), 0});
        prepared.expected.push_back({y / 256, x / 256, (y % 256) * 256 + x % 256, 1});
        if (batch.size() == 4096) { take(writer->append(batch)); batch.clear(); }
    };
    // Keep explicit anchors separate from regular points and their expected cells.
    batch.push_back({.25, .25, 0}); prepared.expected.push_back({0, 0, 0, 1});
    appendSample(profile.extent - 1, profile.extent - 1, true);
    for (U64 i = 0; i < count - 2; ++i) {
        const U64 g = i % profile.groups;
        const U64 grid = profile.extent / 512;
        U64 gx, gy;
        if (profile.groups == 1) gx = gy = 0;
        else if (g == profile.groups - 1) gx = gy = grid - 1;
        else { gx = g % 8; gy = g / 8; }
        const U64 h = mix((i / profile.groups) * 0x9e3779b97f4a7c15ULL + g + 6003);
        const U64 lx = profile.compact ? 7 : h % 512;
        const U64 ly = profile.compact ? 11 : (h >> 32) % 512;
        appendSample(gx * 512 + lx, gy * 512 + ly);
    }
    if (!batch.empty()) take(writer->append(batch));
    prepared.cache.emplace(take(writer->finish())); writer.reset();
    const auto &metadata = prepared.cache->metadata();
    auto location = std::make_shared<DatasetLocation>(); location->storageId = "density-builder-benchmark";
    location->storageDirectory = std::filesystem::path(directory.toStdWString()); location->relativeCachePath = L"source.pc2d";
    prepared.source.cacheId = metadata.id; prepared.source.location = location;
    prepared.source.pointCount = metadata.pointCount;
    prepared.source.minimum = {metadata.bounds.minX, metadata.bounds.minY};
    prepared.source.maximum = {metadata.bounds.maxX, metadata.bounds.maxY};
    require(metadata.pointCount == count && metadata.bounds.minX == .25 && metadata.bounds.minY == .25
        && metadata.bounds.maxX == double(profile.extent) + .25 && metadata.bounds.maxY == double(profile.extent) + .25,
        "prepared source bounds/count mismatch");
    std::sort(prepared.expected.begin(), prepared.expected.end(), [](const Expected &a, const Expected &b) { return a.key() < b.key(); });
    std::size_t used = 0;
    for (const auto entry : prepared.expected) {
        if (used && prepared.expected[used - 1].key() == entry.key()) ++prepared.expected[used - 1].count;
        else prepared.expected[used++] = entry;
    }
    prepared.expected.resize(used);
    for (std::size_t i = 0; i < used; ++i)
        if (!i || prepared.expected[i].tileX != prepared.expected[i - 1].tileX || prepared.expected[i].tileY != prepared.expected[i - 1].tileY) ++prepared.expectedTiles;
    prepared.preparationMs = ms(static_cast<U64>(timer.nsecsElapsed())); return prepared;
}
QJsonObject statsJson(const BuildStatistics &s)
{
    return {{"scanned", n(s.scanned)}, {"eventsWritten", n(s.eventsWritten)}, {"eventsRead", n(s.eventsRead)},
        {"accumulated", n(s.accumulated)}, {"tileTotal", n(s.tileTotal)}, {"scanAllCalls", n(s.scanAllCalls)},
        {"spatialQueries", n(s.spatialQueries)}, {"occupiedGroups", n(s.occupiedGroups)}, {"visitedGroups", n(s.visitedGroups)}, {"tiles", n(s.tiles)},
        {"eventBytesWritten", n(s.eventBytesWritten)}, {"eventBytesRead", n(s.eventBytesRead)}, {"eventWrites", n(s.eventWrites)},
        {"eventReads", n(s.eventReads)}, {"eventOpens", n(s.eventOpens)}, {"workingPeakBytes", n(s.workingPeak)},
        {"metadataPeakBytes", n(s.metadataPeak)}, {"scratchPeakBytes", n(s.scratchPeak)}, {"combinedDiskPeakBytes", n(s.combinedDiskPeak)},
        {"filesPeak", n(s.filesPeak)}, {"handlesPeak", n(s.handlesPeak)}, {"workUnits", n(s.workUnits)},
        {"mappingMs", ms(s.mappingNanoseconds)}, {"sourceOpenMs", ms(s.sourceOpenNanoseconds)}, {"scanMs", ms(s.scanNanoseconds)},
        {"groupingMs", ms(s.groupingNanoseconds)}, {"eventSortMs", ms(s.eventSortNanoseconds)}, {"eventIoMs", ms(s.eventIONanoseconds)},
        {"accumulationMs", ms(s.accumulationNanoseconds)}, {"tileCopyMs", ms(s.tileCopyNanoseconds)},
        {"densityIngestMs", ms(s.densityIngestNanoseconds)}, {"finishMs", ms(s.finishNanoseconds)},
        {"sourceReadBytes", n(s.sourceRead.readBytes)}, {"sourceReadOperations", n(s.sourceRead.readOperations)}, {"sourceSeeks", n(s.sourceRead.seekOperations)},
        {"sourcePointPages", n(s.sourceRead.pointPages)}, {"sourceIndexPages", n(s.sourceRead.indexPages)},
        {"sourceBufferReserveBytes", n(s.sourceRead.bufferBytes)}, {"sourceTraversalFrames", int(s.sourceRead.traversalFrames)},
        {"densityReadBytes", n(s.density.readBytes)}, {"densityWrittenBytes", n(s.density.writtenBytes)}, {"densityFileBytes", n(s.density.outputBytes)},
        {"densityReadOperations", n(s.density.readOperations)}, {"densityWriteOperations", n(s.density.writeOperations)}, {"densityOpenOperations", n(s.density.openOperations)},
        {"densityTotal", n(s.density.total)}, {"densityPhysicalTiles", n(s.density.physicalTiles)}, {"densityCodecPeakBytes", n(s.density.codecPeak)},
        {"densitySortReserveBytes", n(s.density.sortReserve)}, {"densityBufferReserveBytes", n(s.density.bufferReserve)},
        {"densityScratchPeakBytes", n(s.density.scratchPeak)}, {"densityCombinedDiskPeakBytes", n(s.density.combinedDiskPeak)},
        {"densitySubmissions", n(s.density.submissions)}, {"densityMergePasses", n(s.density.mergePasses)}, {"densityWorkUnits", n(s.density.workUnits)},
        {"densityMappingMs", ms(s.density.mappingNanoseconds)}, {"densitySortMs", ms(s.density.sortNanoseconds)},
        {"densityCopyMs", ms(s.density.copyNanoseconds)}, {"densityReadbackMs", ms(s.density.readbackNanoseconds)}};
}
QJsonObject policyJson(const BuildResourcePolicy &p)
{
    return {{"workingBytes", n(p.workingBytes)}, {"metadataBytes", n(p.metadataBytes)}, {"readerBytes", n(p.readerBytes)},
        {"scratchBytes", n(p.scratchBytes)}, {"outputBytes", n(p.outputBytes)}, {"combinedDiskBytes", n(p.combinedDiskBytes)},
        {"files", n(p.files)}, {"handles", int(p.handles)}, {"workUnits", n(p.workUnits)}, {"mappingBoundaryChecks", n(p.mapping.maxBoundaryChecks)},
        {"densityBufferBytes", n(p.densityBufferBytes)}, {"densityCodecBytes", n(p.densityCodecBytes)}, {"densitySortBytes", n(p.densitySortBytes)}};
}
QJsonObject verify(const Prepared &prepared, const PendingDataset &dataset)
{
    auto reader = take(Reader::openReader(dataset.path(), initialResourcePolicy()));
    require(reader->mapping().width() == prepared.width && reader->mapping().height() == prepared.height,
        "D shape differs from independent fixture");
    const auto &metadata = reader->metadata();
    require(metadata.source == prepared.source.cacheId && metadata.sourceMeasurements == prepared.source.pointCount
        && metadata.contract == BuildContract::CompletePointCache && metadata.tileSide == 256, "D provenance differs");
    struct Consumer { const Prepared &p; std::size_t next = 0; U64 tiles = 0, total = 0; bool valid = true; } consumer{prepared};
    auto sink = [](const Tile &tile, void *context) noexcept -> Status {
        auto &c = *static_cast<Consumer *>(context); const auto &p = c.p;
        if (c.next >= p.expected.size()) { c.valid = false; return success(); }
        const auto &first = p.expected[c.next];
        const U64 expectedWidth = std::min<U64>(256, p.width - tile.key.x * 256);
        const U64 expectedHeight = std::min<U64>(256, p.height - tile.key.y * 256);
        if (tile.key.x != first.tileX || tile.key.y != first.tileY || tile.shape.width != expectedWidth || tile.shape.height != expectedHeight
            || tile.counts.size() != expectedWidth * expectedHeight) { c.valid = false; return success(); }
        for (std::size_t local = 0; local < tile.counts.size(); ++local) {
            U64 expected = 0;
            if (c.next < p.expected.size()) {
                const auto &e = p.expected[c.next];
                if (e.tileX == tile.key.x && e.tileY == tile.key.y && e.local == local) { expected = e.count; ++c.next; }
            }
            if (tile.counts[local] != expected) c.valid = false;
            c.total += tile.counts[local];
        }
        ++c.tiles; return success();
    };
    take(reader->forEachTile(+sink, &consumer));
    require(consumer.valid && consumer.next == prepared.expected.size() && consumer.tiles == prepared.expectedTiles
        && consumer.total == prepared.source.pointCount, "independent exact spatial oracle failed");
    U64 implicitZeroTilesChecked = 0;
    // A sparse profile has vacant tiles near the middle. Check a known absent key
    // through the regular API, including its entire decoded zero array.
    const TileKey middle{prepared.width / 512, prepared.height / 512};
    const bool present = std::any_of(prepared.expected.begin(), prepared.expected.end(), [&](const Expected &e) {
        return e.tileX == middle.x && e.tileY == middle.y;
    });
    if (!present && middle.x * 256 < prepared.width && middle.y * 256 < prepared.height) {
        auto tile = take(reader->readTile(middle));
        require(std::all_of(tile.counts.begin(), tile.counts.end(), [](U64 value) { return value == 0; }), "implicit zeros differ");
        ++implicitZeroTilesChecked;
    }
    return {{"exactSpatialCounts", true}, {"physicalTilesChecked", n(consumer.tiles)}, {"nonzeroCellsChecked", n(prepared.expected.size())},
        {"total", n(consumer.total)}, {"implicitZeroTilesChecked", n(implicitZeroTilesChecked)}, {"readerBytes", n(reader->statistics().readBytes)}};
}
QJsonObject trial(const Profile &profile, const Prepared &prepared, const QString &directory, U64 eventCapacity, unsigned repetition)
{
    BuildRequest request; request.source = prepared.source; request.cell = 1; request.tileSide = 256; request.groupSide = 512;
    request.eventCapacity = eventCapacity; request.resources = initialBuildResourcePolicy(); request.scratchParent = directory;
    request.outputPath = directory + QStringLiteral("/map-%1-%2.dmap").arg(eventCapacity).arg(repetition);
    const auto before = sample(); QElapsedTimer timer; timer.start(); auto result = DensityBuilder::build(request);
    const double wallMs = ms(static_cast<U64>(timer.nsecsElapsed())); const auto after = sample();
    QJsonObject row{{"profile", profile.name}, {"repetition", int(repetition)}, {"runClass", profile.name == "irregular64" && eventCapacity == 65536 ? "representative-repeat" : "sample"},
        {"N", n(prepared.source.pointCount)}, {"sourceBytes", n(U64(QFileInfo(QString::fromStdWString(prepared.source.cachePath().wstring())).size()))},
        {"width", n(prepared.width)}, {"height", n(prepared.height)}, {"cellMm", 1}, {"tileSide", 256}, {"groupSide", 512}, {"eventCapacity", n(eventCapacity)},
        {"cachePreparationAndOracleMs", prepared.preparationMs}, {"wallMs", wallMs}, {"processCpuMs", double(after.cpuTicks - before.cpuTicks) / 10000.},
        {"workingSetBeforeBytes", n(before.memory.WorkingSetSize)}, {"workingSetAfterBytes", n(after.memory.WorkingSetSize)},
        {"privateBytesAfter", n(after.memory.PrivateUsage)}, {"processLifetimePeakWorkingSetBytes", n(after.memory.PeakWorkingSetSize)},
        {"processReadBytes", n(after.io.ReadTransferCount - before.io.ReadTransferCount)}, {"processWriteBytes", n(after.io.WriteTransferCount - before.io.WriteTransferCount)},
        {"processReadOperations", n(after.io.ReadOperationCount - before.io.ReadOperationCount)}, {"processWriteOperations", n(after.io.WriteOperationCount - before.io.WriteOperationCount)},
        {"policy", policyJson(request.resources)}};
    if (!result) {
        const auto &error = result.error(); row["status"] = "build-failed";
        row["error"] = QJsonObject{{"code", int(error.code)}, {"phase", int(error.phase)}, {"detail", error.detail}, {"required", n(error.required)}, {"budget", n(error.budget)}};
        return row;
    }
    const auto &stats = result.value().statistics;
    row["statistics"] = statsJson(stats);
    timer.restart();
    try {
        require(stats.scanned == prepared.source.pointCount && stats.eventsWritten == stats.scanned && stats.eventsRead == stats.scanned
            && stats.accumulated == stats.scanned && stats.tileTotal == stats.scanned && stats.density.total == stats.scanned,
            "successful pipeline totals differ");
        require(stats.scanAllCalls == 1 && stats.spatialQueries == 0 && stats.occupiedGroups == stats.visitedGroups
            && stats.occupiedGroups == profile.groups && stats.tiles == prepared.expectedTiles, "successful pipeline traversal differs");
        row["verification"] = verify(prepared, result.value().dataset); row["status"] = "PASS";
    } catch (const std::exception &error) { row["status"] = "verification-failed"; row["diagnostic"] = error.what(); }
    row["verificationMs"] = ms(static_cast<U64>(timer.nsecsElapsed()));
    const U64 rawCounts = prepared.expectedTiles * U64(256) * 256 * 4;
    row["u32DensePhysicalTileBytesOverFinalBytes"] = double(rawCounts) / double(stats.density.outputBytes);
    return row;
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 4) { qInfo("Usage: density_builder_benchmark <existing scratch parent> <new output.json> <points per profile>"); return 2; }
    try {
        bool valid = false; const U64 points = app.arguments()[3].toULongLong(&valid);
        require(valid && points >= 4096, "points must be an integer >=4096");
        const QString parent = QFileInfo(app.arguments()[1]).absoluteFilePath();
        require(QFileInfo(parent).isDir(), "scratch parent is not a directory");
        QFile output(app.arguments()[2]); require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), "output must be a new writable file");
        QJsonObject report{{"schema", "density-builder-06.3A-v1"}, {"environment", environment(parent)},
            {"phaseTimingDefinition", "scan includes grouping; grouping includes event sorting and spool writes. accumulation includes group zeroing/event reads/counting; tile copy and D ingest are separate. eventIo overlaps grouping/accumulation. finish includes D sort/copy/readback. Nested times are not additive; addTile is not Deflate-only."},
            {"coverage", "synthetic integer/rational full-pipeline performance; exact independent sparse oracle after each build; no real scans/cold-HDD/UNC/physical faults"},
            {"componentOperationCoverage", "source: combined open+scan read/seek operations and one reader open. Events: payload read/write calls and successful spool opens; EOF probes/flush/size/remove are excluded. D: logical exact read/write requests, four writer scratch/staged opens plus readback reader open; readback open bytes included. Process counters include all build I/O, with different OS-call granularity."}};
        QJsonArray rows;
        auto saveEvidence = [&] {
            report["rows"] = rows;
            const auto bytes = QJsonDocument(report).toJson();
            require(output.seek(0) && output.resize(0) && output.write(bytes) == bytes.size() && output.flush(), "evidence output write failed");
        };
        report["status"] = "INCOMPLETE"; saveEvidence();
        const std::array profiles{Profile{"compact", 1, 512, true}, Profile{"irregular64", 64, 4096},
            Profile{"irregular256", 256, 16384}, Profile{"sparse64", 64, 524288},
            Profile{"fixed-area-16", 16, 16384}, Profile{"fixed-area-128", 128, 16384}};
        bool allPassed = true;
        for (const auto &profile : profiles) {
            QTemporaryDir directory(parent + "/.density063a-bench-XXXXXX"); require(directory.isValid(), "temporary directory create failed");
            auto prepared = prepare(profile, points, directory.path());
            const unsigned repeats = profile.name == "irregular64" ? 3U : 1U;
            for (unsigned repetition = 1; repetition <= repeats; ++repetition) {
                auto row = trial(profile, prepared, directory.path(), 65536, repetition);
                allPassed = allPassed && row["status"] == "PASS"; rows.append(row); saveEvidence();
                qInfo("%s repeat=%u status=%s wallMs=%.3f", qPrintable(profile.name), repetition, qPrintable(row["status"].toString()), row["wallMs"].toDouble());
            }
            if (profile.name == "irregular64") {
                auto row = trial(profile, prepared, directory.path(), 1024, 1);
                allPassed = allPassed && row["status"] == "PASS"; rows.append(row); saveEvidence();
            }
        }
        QJsonObject representative, sparse, smallChunk;
        for (const auto value : rows) {
            const auto row = value.toObject();
            if (row["profile"] == "irregular64" && row["eventCapacity"] == "65536" && row["repetition"].toInt() == 1) representative = row;
            if (row["profile"] == "irregular64" && row["eventCapacity"] == "1024") smallChunk = row;
            if (row["profile"] == "sparse64") sparse = row;
        }
        auto same = [](const QJsonObject &a, const QJsonObject &b, const char *key) {
            return a["status"] == "PASS" && b["status"] == "PASS" && a["statistics"].toObject()[key] == b["statistics"].toObject()[key];
        };
        const bool emptyArea = same(representative, sparse, "occupiedGroups") && same(representative, sparse, "visitedGroups") && same(representative, sparse, "tiles");
        const bool chunks = same(representative, smallChunk, "tileTotal") && same(representative, smallChunk, "tiles")
            && same(representative, smallChunk, "occupiedGroups") && representative["verification"] == smallChunk["verification"];
        report["pairedChecks"] = QJsonObject{{"emptyAreaDoesNotIncreaseGroupsSpoolsTiles", emptyArea}, {"eventCapacityPreservesExactSpatialOracle", chunks}};
        allPassed = allPassed && emptyArea && chunks;
        report["status"] = allPassed ? "PASS" : "FAIL"; saveEvidence();
        return allPassed ? 0 : 1;
    } catch (const std::exception &error) { qCritical("%s", error.what()); return 1; }
}
