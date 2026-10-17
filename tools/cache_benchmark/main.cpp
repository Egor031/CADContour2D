#include "cache/cache_reader.h"
#include "cache/cache_writer.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStorageInfo>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

using namespace cadcontour;
using namespace cadcontour::cache;

// This executable is the only user of these private observation/build adapters.
// The source-order file deliberately violates canonical Morton order; it is never
// offered to the application, and full validate() is used only on real Writer output.
namespace cadcontour::cache {
class PointStreamSortTestAccess {
public:
    static auto stats(const PointStreamSort &s) { return s.statistics_; }
    static auto phase(const PointStreamSort &s) { return s.phase_; }
    static auto fan(const PointStreamSort &s) { return s.fanIn_; }
    static auto capacity(const PointStreamSort &s) { return s.runCapacity_; }
    static auto block(const PointStreamSort &s) { return s.blockRecords_; }
    static auto fixed(const PointStreamSort &s) { return s.fixedBytes_; }
};
class CacheWriterTestAccess {
public:
    static auto sorter(CacheWriter &w) { return w.sorter_.get(); }
    static auto phase(const CacheWriter &w) { return w.phase_; }
    static auto reserve(const CacheWriter &w) { return w.writerReserve_; }
    static auto peakDisk(const CacheWriter &w) { return w.peakDiskBytes_; }
    static auto buffers(const CacheWriter &w) {
        return (w.page_ ? format::pageSize : 0) + (w.records_ ? format::pointCapacity * sizeof(PointRecord) : 0)
            + (w.entries_ ? format::indexCapacity * sizeof(IndexEntry) : 0);
    }
    static auto openFiles(const CacheWriter &w) { return unsigned(bool(w.staged_)) + unsigned(w.descriptors_ && w.descriptors_->isOpen()); }
    static void beginSourceOrder(CacheWriter &w) { w.sorter_.reset(); }
    static Status sourceRecords(CacheWriter &w, std::span<const PointRecord> records) { return w.consume(records); }
    static Result<PublishedFile> finishSourceOrder(CacheWriter &w, CacheMetadata metadata) {
        w.sorter_.reset();
        w.header_.metadata = metadata;
        if (auto s = w.finishPointPage(); !s) return s.error();
        auto layout = calculateLayout(w.streamCount_, 0);
        if (!layout) return layout.error();
        w.layout_ = layout.value();
        if (auto s = w.buildIndex(); !s) return s.error();
        w.phase_ = Phase::Sealing;
        layout = calculateLayout(w.streamCount_, w.header_.index.pageCount);
        if (!layout) return layout.error();
        w.layout_ = layout.value();
        MutableBytes page{w.page_.get(), format::pageSize};
        if (auto s = encodeHeader(page.first(format::headerSize), w.header_); !s) return s.error();
        if (auto s = w.writeCache(0, page.first(format::headerSize)); !s) return s.error();
        CompletionFooter footer{metadata.id, metadata.pointCount, w.layout_.fileSize, w.pointPages_,
            w.header_.index.pageCount, w.layout_.footerOffset};
        if (auto s = encodeFooter(page.first(format::footerSize), footer); !s) return s.error();
        if (auto s = w.writeCache(w.layout_.footerOffset, page.first(format::footerSize)); !s) return s.error();
        return w.staged_->publish(w.layout_.fileSize, [](QIODevice &d, std::uint64_t size, void *context) {
            return static_cast<CacheWriter *>(context)->validate(d, size);
        }, &w);
    }
};
}
namespace {
using W = CacheWriterTestAccess;
using S = PointStreamSortTestAccess;
constexpr std::uint64_t MiB = 1024 * 1024;
std::atomic<bool> interrupted = false;
BOOL WINAPI interrupt(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { interrupted = true; return TRUE; }
    return FALSE;
}
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(QString("cache error %1/%2/%3: %4").arg(int(result.error().code))
        .arg(int(result.error().phase)).arg(int(result.error().operation)).arg(result.error().detail).toStdString());
    return std::move(result.value());
}
void check(Status result) { take(std::move(result)); }
auto bits(double value) { return std::bit_cast<std::uint64_t>(value); }
double milliseconds(QElapsedTimer &timer) { return double(timer.nsecsElapsed()) / 1000000.; }
void number(QJsonObject &object, const char *name, std::uint64_t value) {
    // Decimal strings preserve uint64 exactly in structured reports.
    object[name] = QString::number(value);
}
QJsonObject stats(const ReadStatistics &s) {
    QJsonObject j;
    number(j, "pointPages", s.pointPages); number(j, "indexPages", s.indexPages);
    number(j, "readBytes", s.readBytes); number(j, "readOperations", s.readOperations);
    number(j, "logicalSeeks", s.seekOperations); number(j, "returnedRecords", s.returnedRecords);
    number(j, "readerBufferBytes", s.bufferBytes);
    return j;
}
struct Source {
    QString distribution;
    std::uint64_t seed;
    Measurement at(std::uint64_t id) const {
        auto j = id;
        if (distribution == "multi" || distribution == "repeated") j /= 2;
        double x = double(j % 8192), y = double(j / 8192);
        if (distribution == "vertical" || (distribution == "multi" && (id & 1))) std::swap(x, y);
        if (distribution == "clustered" || distribution == "sparse") {
            // Stateless fixed-seed permutation, bounded source memory.
            auto value = id + seed + 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
            value ^= value >> 31;
            const auto cluster = value % 4;
            const double spacing = distribution == "sparse" ? 100000. : 1200.;
            x = double((value >> 8) % 1000) / 8 + double(cluster % 2) * spacing;
            y = double((value >> 24) % 1000) / 8 + double(cluster / 2) * spacing;
        }
        return {x, y, id % 997 ? 0. : 1.};
    }
};
struct Query { QString name; std::vector<BoundingBox> boxes; std::uint64_t expected = 0; };
bool matches(Measurement m, const std::vector<BoundingBox> &boxes) {
    return std::any_of(boxes.begin(), boxes.end(), [m](auto b) {
        return b.minX <= m.x && m.x <= b.maxX && b.minY <= m.y && m.y <= b.maxY;
    });
}
std::vector<Query> queries() {
    return {{"local", {{10, 10, 15, 15}}}, {"horizontal", {{0, 100, 1000000, 100}}},
        {"vertical", {{100, 0, 100, 1000000}}},
        {"separated", {{10, 10, 15, 15}, {1000, 10, 1010, 20}, {10, 1000, 20, 1010}}},
        {"overlapping", {{10, 10, 30, 30}, {20, 20, 40, 40}, {10, 10, 30, 30}}},
        {"large", {{0, 0, 4096, 4096}}}, {"scanAll", {}}};
}
struct Observer {
    CacheWriter *writer = nullptr;
    int lastPhase = -1;
    std::uint64_t lastGeneration = 0;
    QElapsedTimer clock;
    qint64 lastNs = 0;
    std::map<QString, double> phaseMs;
    QJsonObject sorter;
    decltype(S::stats(std::declval<const PointStreamSort &>())) lastStatistics{};
    std::uint64_t peakArrays = 0;
    unsigned peakDatasetFiles = 0;
    QString label = "setup";
    Observer() { clock.start(); }
    void sample() {
        int phase = -1;
        std::uint64_t generation = 0;
        if (writer) {
            auto arrays = W::buffers(*writer);
            auto files = W::openFiles(*writer);
            phase = int(W::phase(*writer));
            if (auto s = W::sorter(*writer)) {
                const auto st = S::stats(*s);
                lastStatistics = st;
                arrays += st.liveBufferBytes; files += st.openFiles;
                generation = st.mergeGenerations;
                phase = int(S::phase(*s));
                if (phase != lastPhase || generation != lastGeneration) {
                    number(sorter, "initialRuns", st.initialRuns); number(sorter, "mergeGenerations", generation);
                    number(sorter, "fanIn", S::fan(*s)); number(sorter, "runCapacity", S::capacity(*s));
                    number(sorter, "blockRecords", S::block(*s)); number(sorter, "fixedReserve", S::fixed(*s));
                    number(sorter, "peakBufferBytes", st.peakBufferBytes);
                    number(sorter, "peakPlannedDiskBytes", st.peakPlannedDiskBytes);
                    number(sorter, "peakOpenFiles", st.peakOpenFiles);
                }
            } else phase += 100;
            peakArrays = std::max(peakArrays, arrays);
            peakDatasetFiles = std::max(peakDatasetFiles, files);
        }
        if (phase != lastPhase || generation != lastGeneration) {
            const auto now = clock.nsecsElapsed();
            phaseMs[label] += double(now - lastNs) / 1000000.;
            lastNs = now; lastPhase = phase; lastGeneration = generation;
            label = QString("phase_%1_generation_%2").arg(phase).arg(generation);
        }
    }
    static bool cancel(void *context) noexcept {
        // No allocations in the frequent path; JSON/timing only at phase boundaries.
        auto &self = *static_cast<Observer *>(context);
        try { self.sample(); } catch (...) { return true; }
        return interrupted.load();
    }
    QJsonObject finish() {
        const auto now = clock.nsecsElapsed();
        phaseMs[label] += double(now - lastNs) / 1000000.;
        number(sorter, "initialRuns", lastStatistics.initialRuns);
        number(sorter, "mergeGenerations", lastStatistics.mergeGenerations);
        number(sorter, "readBytes", lastStatistics.readBytes); number(sorter, "writeBytes", lastStatistics.writeBytes);
        number(sorter, "readOperations", lastStatistics.readOperations); number(sorter, "writeOperations", lastStatistics.writeOperations);
        number(sorter, "logicalSeeks", lastStatistics.seekOperations);
        number(sorter, "peakOpenFiles", lastStatistics.peakOpenFiles);
        number(sorter, "peakPlannedDiskBytes", lastStatistics.peakPlannedDiskBytes);
        QJsonObject j; for (auto &[name, ms] : phaseMs) j[name] = ms;
        return j;
    }
};
struct Monitor {
    std::atomic<bool> stop = false;
    std::thread worker;
    std::uint64_t peakWorkingSet = 0, sampledWorkingSet = 0, peakPrivateBytes = 0, peakDisk = 0;
    DWORD peakHandles = 0, initialHandles = 0;
    IO_COUNTERS before{}, after{};
    explicit Monitor(QStringList roots) {
        GetProcessHandleCount(GetCurrentProcess(), &initialHandles);
        GetProcessIoCounters(GetCurrentProcess(), &before);
        worker = std::thread([this, roots] {
            while (!stop) {
                PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
                if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
                    peakWorkingSet = std::max<std::uint64_t>(peakWorkingSet, memory.PeakWorkingSetSize);
                    sampledWorkingSet = std::max<std::uint64_t>(sampledWorkingSet, memory.WorkingSetSize);
                    peakPrivateBytes = std::max<std::uint64_t>(peakPrivateBytes, memory.PrivateUsage);
                }
                DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
                peakHandles = std::max(peakHandles, handles);
                std::uint64_t disk = 0;
                for (const auto &root : roots) {
                    QDirIterator files(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
                    while (files.hasNext()) {
                        const auto path = files.next();
                        // Directory entries can retain size=0 until a Windows writer closes.
                        const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (handle != INVALID_HANDLE_VALUE) {
                            LARGE_INTEGER size{};
                            if (GetFileSizeEx(handle, &size) && size.QuadPart >= 0) disk += std::uint64_t(size.QuadPart);
                            CloseHandle(handle);
                        }
                    }
                }
                peakDisk = std::max(peakDisk, disk);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    void end() { if (worker.joinable()) { stop = true; worker.join(); GetProcessIoCounters(GetCurrentProcess(), &after); } }
    ~Monitor() { end(); }
    QJsonObject json() {
        end(); QJsonObject j;
        number(j, "processLifetimePeakWorkingSetBytes", peakWorkingSet);
        number(j, "sampledPhasePeakWorkingSetBytes", sampledWorkingSet); number(j, "sampledPeakPrivateBytes", peakPrivateBytes);
        number(j, "sampledPeakOwnedDiskBytes", peakDisk); number(j, "sampledPeakProcessHandles", peakHandles);
        number(j, "initialProcessHandles", initialHandles);
        number(j, "processReadTransferBytes", after.ReadTransferCount - before.ReadTransferCount);
        number(j, "processWriteTransferBytes", after.WriteTransferCount - before.WriteTransferCount);
        return j;
    }
};
bool cancelOnly(void *) noexcept { return interrupted.load(); }
QJsonArray scanQueries(const QString &path, const Source &source, const std::vector<Query> &qs,
                      std::uint64_t n, bool canonical, int repetitions)
{
    QJsonArray output;
    QElapsedTimer timer; timer.start();
    auto reader = take(CacheReader::open(path, {MiB, 0, 1}));
    const auto openMs = milliseconds(timer);
    for (const auto &q : qs) {
        for (int run = 0; run < repetitions; ++run) {
            std::uint64_t count = 0, previousId = 0;
            Morton128 previousKey{}; bool previous = false;
            timer.start();
            auto measuredSink = [&](auto records) -> Status { count += records.size(); return success(); };
            const auto read = take(q.boxes.empty() ? reader->scanAll(measuredSink, cancelOnly)
                : reader->scanBoxes(q.boxes, measuredSink, cancelOnly));
            const auto ms = milliseconds(timer);
            require(count == q.expected, "measured query count differs");
            auto j = stats(read); j["query"] = q.name; j["run"] = run; j["ms"] = ms;
            j["openMs"] = openMs;
            j["condition"] = run ? "warm-repeat" : "first-query-after-build-not-cold";
            output.append(j);
            if (run != repetitions - 1) continue;
            count = 0;
            auto sink = [&](auto records) -> Status {
                for (const auto &r : records) {
                    require(r.id < n, "ID outside source");
                    const auto expected = source.at(r.id);
                    require(bits(expected.x) == bits(r.point.x) && bits(expected.y) == bits(r.point.y), "coordinate bits differ");
                    require(q.boxes.empty() || matches(expected, q.boxes), "record outside query");
                    const auto key = canonical ? mortonKey(r.point) : Morton128{};
                    require(!previous || (canonical ? (previousKey < key || (previousKey == key && previousId < r.id))
                                                   : previousId < r.id), "nonunique or unordered result");
                    previous = true; previousKey = key; previousId = r.id; ++count;
                }
                return success();
            };
            take(q.boxes.empty() ? reader->scanAll(sink, cancelOnly) : reader->scanBoxes(q.boxes, sink, cancelOnly));
            require(count == q.expected, "independent source count differs");
        }
    }
    return output;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Explicit headless cache reliability/performance experiment. No system cache eviction.");
    parser.addHelpOption();
    parser.addOptions({{"parent", "Existing output parent; a unique child is created.", "directory"},
        {"temp-parent", "Existing scratch parent (defaults to parent).", "directory"},
        {"report", "New JSON report path, outside temporary directories.", "file"},
        {"points", "Number of 24-byte point records.", "count", "1200000"},
        {"memory-mib", "Writer and validation total working budget.", "MiB", "8"},
        {"disk-mib", "Temporary logical disk budget.", "MiB", "8192"},
        {"distribution", "horizontal|vertical|multi|clustered|sparse|repeated", "name", "multi"},
        {"seed", "Stateless source seed.", "uint64", "314159"},
        {"repeats", "Queries per class (first pass plus warm repeats).", "count", "3"}});
    if (!parser.parse(app.arguments())) {
        std::fprintf(stderr, "%s\n", parser.errorText().toUtf8().constData());
        return 2;
    }
    if (parser.isSet("help") || parser.isSet("help-all")) {
        std::printf("%s", parser.helpText().toUtf8().constData());
        return 0;
    }
    QJsonObject report;
    std::unique_ptr<QTemporaryDir> dataDir, tempDir;
    int exitCode = 1;
    try {
        auto unsignedOption = [&](const char *name) {
            bool ok = false; const auto value = parser.value(name).toULongLong(&ok);
            require(ok, "invalid unsigned option"); return std::uint64_t(value);
        };
        const auto n = unsignedOption("points"), memoryMiB = unsignedOption("memory-mib"), diskMiB = unsignedOption("disk-mib");
        const auto memory = checked::multiply(memoryMiB, MiB), disk = checked::multiply(diskMiB, MiB);
        require(n > 0 && memory && disk, "invalid size or budget");
        const auto repetitions = unsignedOption("repeats");
        require(repetitions >= 2 && repetitions <= 20, "repeats must be in 2..20");
        const Source source{parser.value("distribution"), unsignedOption("seed")};
        require(QStringList{"horizontal", "vertical", "multi", "clustered", "sparse", "repeated"}.contains(source.distribution), "unknown distribution");
        const auto parent = QFileInfo(parser.value("parent")).absoluteFilePath();
        const auto tempParent = parser.isSet("temp-parent") ? QFileInfo(parser.value("temp-parent")).absoluteFilePath() : parent;
        require(parser.isSet("parent") && parser.isSet("report") && QDir(parent).exists() && QDir(tempParent).exists(), "existing parents and report required");
        require(!QFileInfo::exists(parser.value("report")), "report already exists");
        QStorageInfo storage(parent), temporaryStorage(tempParent);
        // Conservative preflight: full selected disk budget on EACH volume plus 10 GiB reserve.
        const auto withReserve = checked::add(*disk, 10 * 1024 * MiB);
        require(withReserve && storage.isValid() && temporaryStorage.isValid()
            && storage.bytesAvailable() >= 0 && temporaryStorage.bytesAvailable() >= 0
            && std::uint64_t(storage.bytesAvailable()) > *withReserve
            && std::uint64_t(temporaryStorage.bytesAvailable()) > *withReserve, "insufficient free space with reserve");
        number(report, "freeBytesBefore", std::uint64_t(storage.bytesAvailable()));
        number(report, "pointCount", n); number(report, "memoryBudgetBytes", *memory); number(report, "diskBudgetBytes", *disk);
        number(report, "seed", source.seed); report["distribution"] = source.distribution;
        report["parent"] = parent; report["temporaryParent"] = tempParent;
        report["qt"] = QT_VERSION_STR; report["compilerMscVer"] = _MSC_VER;
        report["cacheConditions"] = "No cache eviction; Writer/readback warms OS cache. Query timings use counting sink; exact oracle runs separately.";
        dataDir = std::make_unique<QTemporaryDir>(QDir(parent).filePath("cache05-XXXXXX"));
        require(dataDir->isValid(), "cannot create output child");
        QString scratch = dataDir->path();
        if (tempParent != parent) {
            tempDir = std::make_unique<QTemporaryDir>(QDir(tempParent).filePath("cache05-XXXXXX"));
            require(tempDir->isValid(), "cannot create scratch child"); scratch = tempDir->path();
        }
        QStringList roots{dataDir->path()}; if (tempDir) roots.append(tempDir->path());
        SetConsoleCtrlHandler(interrupt, TRUE);
        auto qs = queries();
        for (std::uint64_t id = 0; id < n; ++id) {
            if (id % 65536 == 0) require(!interrupted, "cancelled preparing oracle");
            auto m = source.at(id);
            for (auto &q : qs) if (q.boxes.empty() || matches(m, q.boxes)) ++q.expected;
        }
        const ResourceLimits limits{*memory, *disk, 4};
        const auto path = dataDir->filePath("morton.cache");
        Observer observer;
        Monitor monitor(roots);
        QElapsedTimer timer; timer.start();
        auto writer = take(CacheWriter::create(path, limits, scratch, Observer::cancel, &observer));
        observer.writer = writer.get();
        const auto s = W::sorter(*writer);
        const auto runs = *checked::pageCount(n, S::capacity(*s));
        const auto payload = checked::multiply(n, 40), headers = checked::multiply(runs, 32);
        const auto generation = payload && headers ? checked::add(*payload, *headers) : std::nullopt;
        const auto doubleGeneration = generation ? checked::multiply(*generation, 2) : std::nullopt;
        const auto pages = *checked::pageCount(n, format::pointCapacity);
        auto children = pages, indexPages = std::uint64_t{0};
        while (children) { children = *checked::pageCount(children, format::indexCapacity); indexPages += children; if (children == 1) break; }
        const auto layout = take(calculateLayout(n, indexPages));
        const auto descriptors = checked::multiply(pages, 96);
        const auto writerPeak = descriptors ? checked::add(layout.fileSize, *descriptors) : std::nullopt;
        const auto plannedPeak = doubleGeneration && writerPeak ? checked::add(*doubleGeneration, *writerPeak) : std::nullopt;
        require(plannedPeak && *plannedPeak <= *disk && *doubleGeneration <= *disk - *disk / 2 && *writerPeak <= *disk / 2,
            "predicted dataset exceeds split disk budget");
        number(report, "conservativePlannedPeakBytes", *plannedPeak);
        number(report, "writerReserveBytes", W::reserve(*writer));
        number(report, "pointRecordBytes", *checked::multiply(n, format::recordSize));
        number(report, "cacheBytes", layout.fileSize); number(report, "indexBytes", indexPages * format::pageSize);
        std::array<Measurement, 4096> batch{};
        for (std::uint64_t offset = 0; offset < n;) {
            const auto count = std::size_t(std::min<std::uint64_t>(batch.size(), n - offset));
            for (std::size_t i = 0; i < count; ++i) batch[i] = source.at(offset + i);
            check(writer->append({batch.data(), count})); offset += count;
        }
        auto pending = take(writer->finish());
        report["buildMs"] = milliseconds(timer);
        observer.writer = nullptr;
        report["phaseMs"] = observer.finish(); report["sorter"] = observer.sorter;
        number(report, "observedPeakWorkingArrayBytes", observer.peakArrays);
        number(report, "observedPeakDatasetFileHandles", observer.peakDatasetFiles);
        number(report, "writerPlannedDiskHighWaterBytes", W::peakDisk(*writer));
        require(pending.releaseToOwner() == path, "ownership transfer failed");
        writer.reset();
        report["buildResources"] = monitor.json();
        std::printf("built %llu records\n", static_cast<unsigned long long>(n)); std::fflush(stdout);
        report["mortonQueries"] = scanQueries(path, source, qs, n, true, int(repetitions));
        {
            Monitor validationMonitor(roots);
            auto reader = take(CacheReader::open(path, {MiB, 0, 1}));
            timer.start(); auto v = take(reader->validate(limits, scratch, cancelOnly));
            auto validation = stats(v); validation["ms"] = milliseconds(timer);
            validation["resources"] = validationMonitor.json(); report["validation"] = validation;
        }
        // Estimate incremental canonical-order CPU work independently of ID scratch I/O.
        {
            auto reader = take(CacheReader::open(path, {MiB, 0, 1}));
            QJsonArray orderCost;
            std::uint64_t digest = 0;
            for (int run = 0; run < 3; ++run) {
                for (bool order : {false, true}) {
                    Morton128 previous{}; RecordId id = 0; bool has = false;
                    timer.start();
                    take(reader->scanAll([&](auto records) -> Status {
                        for (auto r : records) {
                            if (order) {
                                const auto key = mortonKey(r.point);
                                require(!has || previous < key || (previous == key && id < r.id), "canonical order");
                                previous = key; has = true;
                            }
                            id = r.id; digest ^= r.id;
                        }
                        return success();
                    }, cancelOnly));
                    QJsonObject j; j["withOrderCheck"] = order; j["run"] = run;
                    j["ms"] = milliseconds(timer); orderCost.append(j);
                }
            }
            report["canonicalOrderCost"] = orderCost; number(report, "orderDigest", digest);
        }
        // Remove only the accepted file in our unique directory, before building baseline.
        require(QFile::remove(path), "cannot remove own Morton cache");
        const auto baselinePath = dataDir->filePath("source-order.baseline");
        {
            Monitor baselineMonitor(roots);
            timer.start();
            auto baseline = take(CacheWriter::create(baselinePath, limits, scratch, cancelOnly));
            W::beginSourceOrder(*baseline);
            CacheMetadata metadata; metadata.id = CacheId::create(); metadata.pointCount = n;
            std::array<PointRecord, 4096> records{};
            for (std::uint64_t offset = 0; offset < n;) {
                require(!interrupted, "baseline cancelled");
                const auto count = std::size_t(std::min<std::uint64_t>(records.size(), n - offset));
                for (std::size_t i = 0; i < count; ++i) {
                    const auto m = source.at(offset + i);
                    records[i] = {offset + i, {m.x, m.y}};
                    if (offset + i == 0) metadata.bounds = {m.x, m.y, m.x, m.y};
                    metadata.bounds.minX = std::min(metadata.bounds.minX, m.x); metadata.bounds.minY = std::min(metadata.bounds.minY, m.y);
                    metadata.bounds.maxX = std::max(metadata.bounds.maxX, m.x); metadata.bounds.maxY = std::max(metadata.bounds.maxY, m.y);
                    metadata.hasNonZeroZ |= m.z != 0;
                }
                check(W::sourceRecords(*baseline, {records.data(), count})); offset += count;
            }
            auto published = take(W::finishSourceOrder(*baseline, metadata));
            report["baselineBuildMs"] = milliseconds(timer);
            require(published.releaseToOwner() == baselinePath, "baseline ownership transfer");
            baseline.reset(); report["baselineResources"] = baselineMonitor.json();
        }
        report["baselineQueries"] = scanQueries(baselinePath, source, qs, n, false, int(repetitions));
        report["status"] = "PASS"; exitCode = 0;
    } catch (const std::exception &error) {
        report["status"] = interrupted ? "CANCELLED" : "FAIL";
        report["error"] = QString::fromUtf8(error.what());
        if (dataDir && dataDir->isValid()) { dataDir->setAutoRemove(false); report["retainedDataDirectory"] = dataDir->path(); }
        if (tempDir && tempDir->isValid()) { tempDir->setAutoRemove(false); report["retainedScratchDirectory"] = tempDir->path(); }
        std::fprintf(stderr, "%s\n", error.what());
    }
    QFile output(parser.value("report"));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    const auto json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (output.write(json) != json.size() || !output.flush()) return 3;
    return exitCode;
}
