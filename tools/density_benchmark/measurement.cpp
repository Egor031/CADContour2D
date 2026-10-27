#include "measurement.h"

#include <QDirIterator>
#include <QDateTime>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStorageInfo>
#include <QSysInfo>

#include <algorithm>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace density {
namespace {
struct ProcessSample {
    bool memoryAvailable = false, ioAvailable = false;
    U64 working = 0, peakWorking = 0, privateBytes = 0;
    IO_COUNTERS io{};
};
ProcessSample sample() {
    ProcessSample s;
    PROCESS_MEMORY_COUNTERS_EX m{};
    s.memoryAvailable = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m), sizeof(m));
    if (s.memoryAvailable) { s.working = m.WorkingSetSize; s.peakWorking = m.PeakWorkingSetSize; s.privateBytes = m.PrivateUsage; }
    s.ioAvailable = GetProcessIoCounters(GetCurrentProcess(), &s.io);
    return s;
}
double ms(QElapsedTimer &t) { return double(t.nsecsElapsed()) / 1000000.; }
QJsonValue metric(bool available, U64 value) { return available ? QJsonValue(decimal(value)) : QJsonValue(QJsonValue::Null); }
QString git(const QString &directory, const QStringList &args) {
    QProcess p; p.setWorkingDirectory(directory); p.start("git", args);
    if (!p.waitForFinished(10000) || p.exitCode()) return "unavailable";
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}
}
U64 workingSet() { auto s = sample(); if (!s.memoryAvailable) throw Failure("resource_refused", "process memory query unavailable"); return s.working; }
U64 directoryBytes(const QString &directory) {
    U64 bytes = 0;
    QDirIterator it(directory, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); require(!it.fileInfo().isSymLink(), "unexpected link in owned experiment directory"); bytes = add(bytes, U64(it.fileInfo().size())); }
    return bytes;
}
QJsonObject preflight(const QString &dir, Limits limits, Plan plan) {
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) throw Failure("resource_refused", "available memory query failed");
    QStorageInfo storage(dir);
    const U64 freeDisk = storage.isValid() && storage.bytesAvailable() >= 0 ? U64(storage.bytesAvailable()) : 0;
    if (!limits.ramBytes || !limits.temporaryBytes || !limits.outputBytes
        || plan.ramBytes > limits.ramBytes || plan.ramBytes > memory.ullAvailPhys
        || plan.temporaryBytes > limits.temporaryBytes || plan.outputBytes > limits.outputBytes
        || add(plan.temporaryBytes, plan.outputBytes) > freeDisk)
        throw Failure("resource_refused", "planned memory/temporary/output demand exceeds budget or currently available resources");
    return {{"availableRamBytes", decimal(memory.ullAvailPhys)}, {"availableDiskBytes", decimal(freeDisk)},
        {"ramBudgetBytes", decimal(limits.ramBytes)}, {"temporaryBudgetBytes", decimal(limits.temporaryBytes)},
        {"outputBudgetBytes", decimal(limits.outputBytes)}, {"plannedRamBytes", decimal(plan.ramBytes)},
        {"plannedTemporaryBytes", decimal(plan.temporaryBytes)}, {"plannedOutputBytes", decimal(plan.outputBytes)}};
}
void resourceCheckpoint(Limits limits, const QString &dir, bool cancelled) {
    if (cancelled) throw Failure("cancelled", "experiment cancelled");
    if (workingSet() > limits.ramBytes || directoryBytes(dir) > add(limits.outputBytes, limits.temporaryBytes))
        throw Failure("resource_refused", "observed process memory or experiment disk usage exceeds budget");
}
QJsonObject environment(const QString &source, const QString &output, const QString &storageLabel) {
    MEMORYSTATUSEX m{}; m.dwLength = sizeof(m); const bool mem = GlobalMemoryStatusEx(&m);
    QStorageInfo disk(output);
    QFile executable(QCoreApplication::applicationFilePath());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const bool hashed = executable.open(QIODevice::ReadOnly) && hash.addData(&executable);
    return {{"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"buildHead", DENSITY_BUILD_HEAD}, {"runtimeHead", git(source, {"rev-parse", "HEAD"})},
        {"toolExecutableSha256", hashed ? QJsonValue(QString::fromLatin1(hash.result().toHex())) : QJsonValue(QJsonValue::Null)},
        {"gitStatus", git(source, {"status", "--porcelain=v1"})}, {"compiler", DENSITY_COMPILER},
        {"msvcFullVersion", decimal(_MSC_FULL_VER)}, {"buildConfiguration", DENSITY_BUILD_CONFIG},
        {"architecture", QSysInfo::buildCpuArchitecture()}, {"qtVersion", qVersion()},
        {"os", QSysInfo::prettyProductName()}, {"cpu", qEnvironmentVariable("PROCESSOR_IDENTIFIER")},
        {"physicalRamBytes", metric(mem, m.ullTotalPhys)}, {"volume", disk.rootPath()},
        {"fileSystem", QString::fromUtf8(disk.fileSystemType())}, {"volumeDevice", QString::fromUtf8(disk.device())},
        {"storageLabel", storageLabel.isEmpty() ? "unspecified; media model/type not detected" : storageLabel},
        {"physicalDiskIo", QJsonValue::Null}, {"filesystemCache", "uncontrolled; never claimed cold"},
        {"memoryDefinition", "Windows PeakWorkingSetSize: process-lifetime resident high-water mark; privateBytes is a snapshot"},
        {"ioDefinition", "GetProcessIoCounters deltas: process logical transfers/operations, NOT physical disk I/O"}};
}
QJsonObject cacheStatistics(const cadcontour::cache::ReadStatistics &s) {
    return {{"readBytes", decimal(s.readBytes)}, {"readOperations", decimal(s.readOperations)},
        {"seekOperations", decimal(s.seekOperations)}, {"indexPages", decimal(s.indexPages)},
        {"pointPages", decimal(s.pointPages)}, {"returnedRecords", decimal(s.returnedRecords)},
        {"bufferBytes", decimal(s.bufferBytes)}, {"traversalFrames", decimal(s.traversalFrames)}};
}
QJsonObject ioJson(const FileIo &s) {
    return {{"readBytes", decimal(s.readBytes)}, {"writeBytes", decimal(s.writeBytes)},
        {"readOperations", decimal(s.reads)}, {"writeOperations", decimal(s.writes)}};
}
QJsonObject measure(const QString &phase, const QString &dataset, const QString &dir,
                    const std::function<QJsonObject(FileIo &)> &operation, const std::function<bool()> &verify) {
    QJsonObject row{{"phase", phase}, {"dataset", dataset}, {"status", "ok"}, {"correctness", QJsonValue::Null}};
    FileIo io;
    auto before = sample(); QElapsedTimer total, timer; total.start(); timer.start();
    try { row["details"] = operation(io); }
    catch (const Failure &e) { row["status"] = e.status; row["diagnostic"] = e.what(); }
    catch (const std::bad_alloc &) { row["status"] = "resource_refused"; row["diagnostic"] = "allocation failed"; }
    catch (const std::exception &e) { row["status"] = "error"; row["diagnostic"] = e.what(); }
    row["elapsedMs"] = ms(timer);
    const auto after = sample();
    row["fileIo"] = ioJson(io);
    row["fileIoDefinition"] = "Actual successful bytes and attempted QIODevice calls in fixture wrappers only; CacheReader has separate statistics";
    row["workingSetBeforeBytes"] = metric(before.memoryAvailable, before.working);
    row["workingSetAfterBytes"] = metric(after.memoryAvailable, after.working);
    row["processPeakWorkingSetBytes"] = metric(after.memoryAvailable, after.peakWorking);
    row["privateBytesAfter"] = metric(after.memoryAvailable, after.privateBytes);
    const bool available = before.ioAvailable && after.ioAvailable;
    row["processIo"] = QJsonObject{{"readBytes", metric(available, after.io.ReadTransferCount - before.io.ReadTransferCount)},
        {"writeBytes", metric(available, after.io.WriteTransferCount - before.io.WriteTransferCount)},
        {"readOperations", metric(available, after.io.ReadOperationCount - before.io.ReadOperationCount)},
        {"writeOperations", metric(available, after.io.WriteOperationCount - before.io.WriteOperationCount)}};
    row["physicalDiskIo"] = QJsonValue::Null;
    row["temporaryPeakBytes"] = QJsonValue::Null; // No filesystem sampler: do not invent a high-water mark.
    row["ownedFilesAfterBytes"] = decimal(directoryBytes(dir));
    timer.restart();
    if (row["status"] == "ok" && verify) {
        try { row["correctness"] = verify(); if (!row["correctness"].toBool()) row["status"] = "incorrect"; }
        catch (const Failure &e) { row["correctness"] = false; row["status"] = e.status; row["diagnostic"] = e.what(); }
        catch (const std::bad_alloc &) { row["correctness"] = false; row["status"] = "resource_refused"; row["diagnostic"] = "verification allocation failed"; }
        catch (const std::exception &e) { row["correctness"] = false; row["status"] = "verification_error"; row["diagnostic"] = e.what(); }
    }
    row["verificationMs"] = ms(timer); row["totalElapsedMs"] = ms(total);
    return row;
}
QByteArray csv(const QJsonArray &rows) {
    const QStringList keys{"dataset", "phase", "repetition", "runClass", "status", "correctness", "elapsedMs",
        "verificationMs", "totalElapsedMs", "processPeakWorkingSetBytes", "ownedFilesAfterBytes", "fileIo", "processIo", "details", "diagnostic"};
    auto quote = [](QString value) { value.replace('"', "\"\""); return '"' + value + '"'; };
    QByteArray bytes = keys.join(',').toUtf8() + '\n';
    for (auto v : rows) {
        auto row = v.toObject(); QStringList fields;
        for (const auto &key : keys) {
            const auto item = row[key]; QString value;
            if (item.isObject()) value = QString::fromUtf8(QJsonDocument(item.toObject()).toJson(QJsonDocument::Compact));
            else if (item.isBool()) value = item.toBool() ? "true" : "false";
            else if (item.isDouble()) value = QString::number(item.toDouble(), 'g', 17);
            else if (item.isString()) value = item.toString();
            fields.append(quote(value));
        }
        bytes += fields.join(',').toUtf8() + '\n';
    }
    return bytes;
}
QJsonObject summary(const QJsonArray &rows) {
    // First run is deliberately excluded. Every raw repetition remains in JSON/CSV.
    QMap<QString, std::vector<double>> groups;
    for (auto v : rows) {
        auto r = v.toObject();
        if (r["status"] == "ok" && r["runClass"] == "subsequent")
            groups[r["dataset"].toString() + "/" + r["phase"].toString()].push_back(r["elapsedMs"].toDouble());
    }
    QJsonObject out;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        auto a = it.value(); std::sort(a.begin(), a.end());
        const auto n = a.size(); const double median = n % 2 ? a[n/2] : (a[n/2-1] + a[n/2]) / 2;
        double mean = 0, variance = 0; for (auto x : a) mean += x / double(n);
        for (auto x : a) variance += (x - mean) * (x - mean);
        out[it.key()] = QJsonObject{{"n", int(n)}, {"minMs", a.front()}, {"medianMs", median},
            {"maxMs", a.back()}, {"meanMs", mean}, {"sampleStddevMs", n > 1 ? QJsonValue(std::sqrt(variance / double(n-1))) : QJsonValue(QJsonValue::Null)}};
    }
    return out;
}
} // namespace density
