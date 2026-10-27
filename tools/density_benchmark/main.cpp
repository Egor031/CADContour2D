#include "dataset.h"
#include "measurement.h"
#include "oracle.h"
#include "generator.h"
#include "cache/cache_writer.h"

#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QtEndian>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace density;
namespace cache = cadcontour::cache;
namespace {
std::atomic<bool> interrupted = false;
BOOL WINAPI interrupt(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { interrupted = true; return TRUE; }
    return FALSE;
}
bool cancelled(void *) noexcept { return interrupted; }
template<class T> T take(cache::Result<T> result) {
    if (!result) {
        const auto &e = result.error();
        throw Failure(e.code == cache::ErrorCode::Cancelled ? "cancelled"
            : e.code == cache::ErrorCode::ResourceExhausted ? "resource_refused" : "cache_error",
            QString("CacheReader/Writer %1/%2/%3: %4").arg(int(e.code)).arg(int(e.phase)).arg(int(e.operation)).arg(e.detail));
    }
    return std::move(result.value());
}
QJsonObject jsonFile(const QString &path, FileIo &io) {
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(readFile(path, 4 * MiB, io), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "invalid JSON document: " + path);
    return document.object();
}
U64 argument(const QCommandLineParser &p, const char *key) {
    bool ok = false; auto n = p.value(key).toULongLong(&ok);
    require(ok, QString("invalid integer option: %1").arg(key)); return n;
}
struct Options {
    QString dir, scenario, externalCache, storage;
    U64 side = 8, gridSide = 3, seed = 6001, repeats = 4;
    double cell = 1;
    Limits limits;
};
void checkMemory(const Options &o) {
    if (interrupted) throw Failure("cancelled", "experiment cancelled");
    if (workingSet() > o.limits.ramBytes) throw Failure("resource_refused", "observed working set exceeds RAM budget");
}
void append(QJsonArray &rows, QJsonObject row) {
    rows.append(row);
    if (row["status"] != "ok") throw Failure(row["status"].toString(), row["diagnostic"].toString("correctness verification failed"));
}
void output(const QString &dir, const QJsonObject &report) {
    FileIo io;
    writeNew(QDir(dir).filePath("results.json"), QJsonDocument(report).toJson(), io);
    writeNew(QDir(dir).filePath("results.csv"), csv(report["measurements"].toArray()), io);
}
QJsonObject fileFingerprint(const Options &o, const QString &dir, const Spec &s) {
    FileIo io;
    return fingerprint(s, [&](TileKey k) {
        checkMemory(o); return decode(readFile(tilePath(dir, ordinalOf(s, k)), 32 + U64(s.side) * s.side * 8, io));
    });
}
void digestRecords(QCryptographicHash &hash, std::span<const cache::PointRecord> records) {
    for (const auto &r : records) {
        char bytes[24];
        qToLittleEndian<U64>(r.id, bytes);
        qToLittleEndian<U64>(std::bit_cast<U64>(r.point.x), bytes + 8);
        qToLittleEndian<U64>(std::bit_cast<U64>(r.point.y), bytes + 16);
        hash.addData(QByteArrayView(bytes, 24));
    }
}
// Read-only preparation and validation, NOT a candidate rasterization algorithm.
void observeCache(const Options &o, QJsonObject &input, QJsonArray &rows, bool prepare) {
    const QString path = input["cachePath"].toString(), name = input["name"].toString();
    std::unique_ptr<cache::CacheReader> reader;
    append(rows, measure("cache_open", name, o.dir, [&](FileIo &) {
        checkMemory(o);
        reader = take(cache::CacheReader::open(path, {8 * MiB, 0, 1}, cancelled));
        return QJsonObject{{"cacheReader", cacheStatistics(reader->statistics())},
            {"pointCount", decimal(reader->metadata().pointCount)},
            {"cacheFileBytes", decimal(U64(QFileInfo(path).size()))}};
    }));
    auto grid = prepare && !input.contains("grid") ? makeGrid(reader->metadata().bounds, o.cell)
        : gridFromJson(input["grid"].toObject());
    input["grid"] = toJson(grid);
    input["pointCount"] = decimal(reader->metadata().pointCount);
    const bool smallInput = grid.width * grid.height <= 4096 && reader->metadata().pointCount <= 100000;
    std::vector<U64> reference;
    if (input.contains("expectedCounts")) reference = countsFromJson(input["expectedCounts"].toArray());
    QString digest;
    U64 seen = 0;
    QJsonObject scanStats;
    auto row = measure("cache_scan_input", name, o.dir, [&](FileIo &) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        auto s = take(reader->scanAll([&](std::span<const cache::PointRecord> batch) {
            checkMemory(o); seen = add(seen, batch.size()); digestRecords(hash, batch); return cache::success();
        }, cancelled));
        digest = QString::fromLatin1(hash.result().toHex()); scanStats = cacheStatistics(s);
        return QJsonObject{{"cacheReader", scanStats}, {"pointDigest", digest},
            {"seenPoints", decimal(seen)}, {"digestOrder", "physical canonical Morton128/RecordId; LE id/x-bits/y-bits"}};
    }, [&] {
        if (seen != reader->metadata().pointCount || (!prepare && digest != input["pointDigest"].toString())) return false;
        if (!smallInput) return true; // Digest/count only; explicitly not an independent per-cell proof.
        std::vector<U64> counts(static_cast<std::size_t>(grid.width * grid.height));
        U64 verified = 0;
        const auto labels = input["expectedCells"].toArray();
        take(reader->scanAll([&](std::span<const cache::PointRecord> batch) {
            checkMemory(o);
            for (auto r : batch) {
                const auto label = oracleCell(grid, r.point);
                if (!labels.isEmpty()) require(r.id < U64(labels.size())
                    && label == integer({{"n", labels[static_cast<qsizetype>(r.id)]}}, "n"), "manual record/cell oracle mismatch");
                counts[label] = add(counts[label], 1); ++verified;
            }
            return cache::success();
        }, cancelled));
        input["verificationCacheReader"] = cacheStatistics(reader->statistics());
        if (!reference.empty() && reference != counts) return false;
        if (verified != seen) return false;
        U64 total = 0; for (auto n : counts) total = add(total, n);
        if (total != seen) return false;
        input["expectedCounts"] = countsJson(counts);
        return true;
    });
    auto details = row["details"].toObject();
    details["oracle"] = smallInput ? "independent interval enumeration; outside timed scan" : "unavailable: large input; count/digest only";
    details["verificationCacheReader"] = input.value("verificationCacheReader");
    row["details"] = details;
    append(rows, row);
    input["pointDigest"] = digest;
    input["oracleAvailable"] = smallInput;
}
QJsonObject writeCache(const Options &o, const QString &name, const Grid &grid,
                       const std::function<void(const std::function<void(cache::Measurement)> &)> &produce,
                       const std::vector<U64> &labels, QJsonArray &rows) {
    const QString cachePath = QDir(o.dir).filePath(name + ".pc2dcache");
    QJsonObject input{{"name", name}, {"cachePath", cachePath}, {"grid", toJson(grid)},
        {"expectedCells", countsJson(labels)}};
    std::vector<U64> reference(static_cast<std::size_t>(grid.width * grid.height));
    append(rows, measure("cache_input_write", name, o.dir, [&](FileIo &) {
        auto writer = take(cache::CacheWriter::create(cachePath, {8 * MiB, o.limits.temporaryBytes, 4},
            QDir(o.dir).filePath("scratch"), cancelled));
        std::vector<cache::Measurement> batch; batch.reserve(1024);
        U64 ordinal = 0;
        produce([&](cache::Measurement p) {
            checkMemory(o);
            const U64 label = oracleCell(grid, {p.x, p.y});
            if (!labels.empty()) require(ordinal < labels.size() && label == labels[ordinal], "literal oracle mismatch before cache writing");
            reference[label] = add(reference[label], 1); ++ordinal;
            batch.push_back(p);
            if (batch.size() == 1024) { take(writer->append(batch)); batch.clear(); }
        });
        take(writer->append(batch));
        auto pending = take(writer->finish()); pending.releaseToOwner();
        return QJsonObject{{"points", decimal(ordinal)}, {"outputFileBytes", decimal(U64(QFileInfo(cachePath).size()))},
            {"writerLogicalIo", QJsonValue::Null}, {"temporaryFilesAfterBytes", decimal(directoryBytes(QDir(o.dir).filePath("scratch")))}};
    }));
    input["expectedCounts"] = countsJson(reference);
    observeCache(o, input, rows, true);
    return input;
}
Plan estimate(const Options &o, const std::vector<Spec> &specs) {
    // Deliberately conservative for six bounded synthetic caches, reports and file allocation overhead.
    Plan p{48 * MiB + multiply(multiply(o.side, o.side), 64), 16 * MiB,
        add(64 * MiB, multiply(o.repeats, 2 * MiB))};
    for (const auto &s : specs) p.outputBytes = add(p.outputBytes, add(inputBytes(s), multiply(tileCount(s), 4096)));
    return p;
}
QJsonObject prepareInputs(const Options &o, const std::vector<Spec> &specs, QJsonArray &rows) {
    require(QDir().mkdir(QDir(o.dir).filePath("scratch")), "cannot create owned scratch directory");
    QJsonArray datasets, rasters;
    for (const auto &s : specs) {
        auto dir = QDir(o.dir).filePath(s.name);
        auto expected = fingerprint(s, [&](TileKey k) { checkMemory(o); return generate(s, k); });
        append(rows, measure("count_input_write", s.name, o.dir, [&](FileIo &io) {
            writeInputs(dir, s, io, [&] { checkMemory(o); });
            return QJsonObject{{"inputBytes", decimal(inputBytes(s))}, {"fingerprint", expected}};
        }, [&] { return expected == fileFingerprint(o, dir, s); }));
        auto item = toJson(s); item["fingerprint"] = expected; item["directory"] = dir; datasets.append(item);
    }
    for (const auto &c : rasterCases()) {
        rasters.append(writeCache(o, c.name, c.grid, [&](const auto &sink) { for (auto p : c.points) sink(p); }, c.expectedCells, rows));
    }
    FileIo io;
    const QString fixtureRoot = QStringLiteral(DENSITY_SOURCE_DIR "/tests/fixtures/synthetic/");
    auto geometryBytes = readFile(fixtureRoot + "geometry/rectangle.json", MiB, io);
    auto scanBytes = readFile(fixtureRoot + "scans/clean_horizontal.json", MiB, io);
    const auto geometry = synthetic::parseGeometry(geometryBytes);
    const auto scan = synthetic::parseScan(scanBytes);
    auto generated = writeCache(o, "synthetic-rectangle-horizontal", makeGrid({0,0,100,60}, 10), [&](const auto &sink) {
        synthetic::generate(geometry, scan, [&](synthetic::Point p) { sink(cache::Measurement{p.x, p.y, 0}); });
    }, {}, rows);
    generated["generatorVersion"] = synthetic::generatorVersion;
    generated["geometrySha256"] = QString::fromLatin1(QCryptographicHash::hash(geometryBytes, QCryptographicHash::Sha256).toHex());
    generated["scanSha256"] = QString::fromLatin1(QCryptographicHash::hash(scanBytes, QCryptographicHash::Sha256).toHex());
    generated["generatorSeed"] = decimal(scan.seed); rasters.append(generated);
    if (!o.externalCache.isEmpty()) {
        QJsonObject external{{"name", "external-cache"}, {"cachePath", QFileInfo(o.externalCache).absoluteFilePath()}};
        observeCache(o, external, rows, true); rasters.append(external);
    }
    QJsonObject inputs{{"datasets", datasets}, {"rasters", rasters}, {"schema", "density-experiment-inputs-1"}};
    writeNew(QDir(o.dir).filePath("inputs.json"), QJsonDocument(inputs).toJson(), io);
    return inputs;
}
void repeat(const Options &o, const QJsonObject &inputs, QJsonArray &rows) {
    for (auto v : inputs["datasets"].toArray()) {
        auto item = v.toObject(); auto s = specFromJson(item); auto dir = item["directory"].toString();
        FileIo manifestIo;
        require(toJson(s) == jsonFile(QDir(dir).filePath("manifest.json"), manifestIo), "count manifest mismatch");
        for (const auto &[name, plan] : readPlans(s)) {
            U64 returned = 0;
            append(rows, measure("count_input_read_" + name, s.name, o.dir, [&](FileIo &io) {
                readInputs(dir, s, plan, [&](const Tile &t) { returned = add(returned, t.counts.size()); },
                    io, [&] { checkMemory(o); });
                return QJsonObject{{"returnedCells", decimal(returned)}, {"request", toJson(plan)}};
            }, [&] {
                U64 checkedCells = 0;
                FileIo verificationIo;
                readInputs(dir, s, plan, [&](const Tile &t) {
                    require(t == generate(s, t.key), "count input differs from deterministic reference");
                    checkedCells = add(checkedCells, t.counts.size());
                }, verificationIo, [&] { checkMemory(o); });
                return checkedCells == returned && fileFingerprint(o, dir, s) == item["fingerprint"].toObject();
            }));
        }
    }
    for (auto v : inputs["rasters"].toArray()) { auto input = v.toObject(); observeCache(o, input, rows, false); }
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); SetConsoleCtrlHandler(interrupt, TRUE);
    QCoreApplication::setApplicationName("density_benchmark");
    QCommandLineParser p; p.setApplicationDescription("06.0A experimental input/measurement harness; no storage/raster candidates"); p.addHelpOption();
    p.addOptions({{"output-dir", "New experiment directory; existing paths are refused", "path"},
        {"scenario", "all or one count scenario", "name", "all"},
        {"tile-size", "Count tile side (4..1024)", "cells", "8"},
        {"grid-side", "Tiles along an ordinary scenario side (>=2)", "tiles", "3"},
        {"seed", "Count generator seed", "uint64", "6001"},
        {"repeats", "Fresh worker processes reading identical inputs (1..50)", "n", "4"},
        {"ram-mib", "Process working memory budget", "MiB", "128"},
        {"temp-mib", "Scratch space budget", "MiB", "128"},
        {"output-mib", "Retained output budget", "MiB", "128"},
        {"cache", "Optional existing Point Cache (opened read-only)", "path"},
        {"cell", "External cache cell in mm", "mm", "1"},
        {"storage-label", "Operator supplied media model/type/topology", "label"},
        {"worker", "Internal repetition index; reads already prepared inputs", "index"}});
    p.process(app);
    QJsonObject report{{"schema", "density-experiment-results-1"}, {"status", "error"}};
    QJsonArray rows;
    QString owned;
    QString workerResultFile;
    QElapsedTimer total; total.start();
    try {
        require(!p.value("output-dir").isEmpty(), "--output-dir is required");
        Options o; o.dir = QFileInfo(p.value("output-dir")).absoluteFilePath(); o.scenario = p.value("scenario");
        o.side = argument(p, "tile-size"); o.gridSide = argument(p, "grid-side"); o.seed = argument(p, "seed"); o.repeats = argument(p, "repeats");
        o.limits = {multiply(argument(p, "ram-mib"), MiB), multiply(argument(p, "temp-mib"), MiB), multiply(argument(p, "output-mib"), MiB)};
        bool cellOk = false; o.cell = p.value("cell").toDouble(&cellOk);
        require(cellOk && std::isfinite(o.cell) && o.cell > 0, "invalid --cell");
        require(o.side >= 4 && o.side <= 1024 && o.gridSide >= 2 && o.repeats >= 1 && o.repeats <= 50, "invalid tile/grid/repeat options");
        o.externalCache = p.value("cache"); o.storage = p.value("storage-label");
        std::vector<Spec> specs;
        for (const auto &name : o.scenario == "all" ? scenarioNames() : QStringList{o.scenario})
            specs.push_back(makeSpec(name, static_cast<std::uint32_t>(o.side), o.gridSide, o.seed));
        if (p.isSet("worker")) {
            FileIo io; auto marker = jsonFile(QDir(o.dir).filePath("owner.json"), io);
            require(marker["schema"] == "density-experiment-owner-1", "worker requires owned experiment inputs");
            auto inputs = jsonFile(QDir(o.dir).filePath("inputs.json"), io);
            const U64 index = argument(p, "worker"); require(index < o.repeats, "worker index outside repetitions");
            workerResultFile = QDir(o.dir).filePath(QString("repeat-%1.json").arg(index));
            preflight(o.dir, o.limits, {48 * MiB + multiply(multiply(o.side,o.side),64), 0, 2 * MiB});
            repeat(o, inputs, rows);
            for (qsizetype rindex = 0; rindex < rows.size(); ++rindex) {
                auto r = rows[rindex].toObject(); r["repetition"] = int(index);
                r["runClass"] = index ? "subsequent" : "first"; rows[rindex] = r;
            }
            QJsonObject worker{{"status", "ok"}, {"measurements", rows}};
            writeNew(workerResultFile, QJsonDocument(worker).toJson(), io);
            return 0;
        }
        require(!QFileInfo::exists(o.dir) && QDir().mkdir(o.dir), "--output-dir must not exist and its parent must exist");
        owned = o.dir; FileIo io;
        writeNew(QDir(o.dir).filePath("owner.json"), QJsonDocument(QJsonObject{{"schema", "density-experiment-owner-1"}}).toJson(), io);
        report["environment"] = environment(DENSITY_SOURCE_DIR, o.dir, o.storage);
        report["parameters"] = QJsonObject{{"tileSide", decimal(o.side)}, {"gridSide", decimal(o.gridSide)}, {"seed", decimal(o.seed)},
            {"repeats", decimal(o.repeats)}, {"scenario", o.scenario}, {"externalCache", o.externalCache}, {"externalCellMm", o.cell},
            {"ramBudgetBytes", decimal(o.limits.ramBytes)}, {"temporaryBudgetBytes", decimal(o.limits.temporaryBytes)}, {"outputBudgetBytes", decimal(o.limits.outputBytes)}};
        report["preflight"] = preflight(o.dir, o.limits, estimate(o, specs));
        const auto inputs = prepareInputs(o, specs, rows); report["inputs"] = inputs;
        resourceCheckpoint(o.limits, o.dir, interrupted);
        for (U64 i = 0; i < o.repeats; ++i) {
            QProcess worker;
            auto args = app.arguments().mid(1); args << "--worker" << decimal(i);
            worker.start(app.applicationFilePath(), args);
            require(worker.waitForStarted(10000), "cannot start repetition worker");
            while (!worker.waitForFinished(100)) {
                if (interrupted) { worker.terminate(); if (!worker.waitForFinished(5000)) { worker.kill(); worker.waitForFinished(5000); } throw Failure("cancelled", "parent cancelled repetition"); }
            }
            const QString resultPath = QDir(o.dir).filePath(QString("repeat-%1.json").arg(i));
            if (!QFileInfo::exists(resultPath))
                throw Failure("worker_error", QString::fromUtf8(worker.readAllStandardError()));
            auto result = jsonFile(resultPath, io);
            for (auto v : result["measurements"].toArray()) rows.append(v);
            if (result["status"] != "ok") throw Failure(result["status"].toString(), result["diagnostic"].toString());
            if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode())
                throw Failure("worker_error", QString::fromUtf8(worker.readAllStandardError()));
        }
        report["status"] = "ok"; report["readiness"] = "READY WITH LIMITATIONS";
        report["subsequentStatistics"] = summary(rows);
        report["scope"] = "Input preparation/readback and measurement infrastructure only; no candidate performance comparison";
        report["temporaryFilesAfterBytes"] = decimal(directoryBytes(QDir(o.dir).filePath("scratch")));
        report["outputFilesBeforeReportsBytes"] = decimal(directoryBytes(o.dir));
        require(directoryBytes(o.dir) + 8 * MiB <= o.limits.outputBytes, "retained outputs leave insufficient report budget");
    } catch (const Failure &e) { report["status"] = e.status; report["diagnostic"] = e.what(); }
      catch (const std::bad_alloc &) { report["status"] = "resource_refused"; report["diagnostic"] = "allocation failed"; }
      catch (const std::exception &e) { report["status"] = "error"; report["diagnostic"] = e.what(); }
    report["measurements"] = rows; report["wholeOperationElapsedMs"] = double(total.nsecsElapsed()) / 1000000.;
    try {
        if (!workerResultFile.isEmpty()) {
            FileIo io; writeNew(workerResultFile, QJsonDocument(report).toJson(), io);
        } else if (!owned.isEmpty()) output(owned, report);
    }
    catch (const std::exception &e) { std::fprintf(stderr, "report write failure: %s\n", e.what()); return 1; }
    if (report["status"] != "ok") { std::fprintf(stderr, "%s: %s\n", qPrintable(report["status"].toString()), qPrintable(report["diagnostic"].toString())); return 1; }
    std::printf("READY WITH LIMITATIONS: %s/results.json and results.csv\n", qPrintable(owned));
    return 0;
}
