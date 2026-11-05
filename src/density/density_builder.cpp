#include "density_builder.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStringView>
#include <QTemporaryDir>
#include <zlib.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>

namespace cadcontour::density {
namespace {
constexpr U64 ioBytes = 65536;
static_assert(ioBytes <= std::numeric_limits<uInt>::max());
constexpr U64 fixedBookkeeping = 65536;
struct Failure { BuildError error; };
BuildError error(BuildErrorCode code, BuildPhase phase, const char *detail)
{ return {code, phase, QString::fromLatin1(detail).left(256)}; }
[[noreturn]] void fail(BuildErrorCode code, BuildPhase phase, const char *detail)
{ throw Failure{error(code, phase, detail)}; }
void require(bool value, BuildErrorCode code, BuildPhase phase, const char *detail)
{ if (!value) fail(code, phase, detail); }
U64 add(U64 a, U64 b, BuildPhase phase)
{
    auto result = cache::checked::add(a, b);
    if (!result) fail(BuildErrorCode::Overflow, phase, "uint64 addition overflow");
    return *result;
}
U64 multiply(U64 a, U64 b, BuildPhase phase)
{
    auto result = cache::checked::multiply(a, b);
    if (!result) fail(BuildErrorCode::Overflow, phase, "uint64 multiplication overflow");
    return *result;
}
std::size_t size(U64 n, BuildPhase phase)
{
    auto result = cache::checked::memorySize(n);
    require(result && n <= static_cast<U64>(std::numeric_limits<qsizetype>::max()),
        BuildErrorCode::ResourceRefused, phase, "Memory size unrepresentable");
    return *result;
}
void budget(U64 need, U64 limit, BuildPhase phase, const char *detail)
{
    if (need > limit) {
        auto e = error(BuildErrorCode::ResourceRefused, phase, detail);
        e.required = need; e.budget = limit; throw Failure{std::move(e)};
    }
}
bool same(double a, double b) { return std::bit_cast<U64>(a) == std::bit_cast<U64>(b); }
bool same(ModelPoint a, ModelPoint b) { return same(a.x, b.x) && same(a.y, b.y); }
bool pathSeparator(QChar c) noexcept { return c == u'/' || c == u'\\'; }
bool qualifiedDrive(QStringView path) noexcept
{
    if (path.size() < 3) return false;
    const auto drive = path[0].unicode();
    return ((drive >= u'A' && drive <= u'Z') || (drive >= u'a' && drive <= u'z'))
        && path[1] == u':' && pathSeparator(path[2]);
}
// The leading UNC separators (or canonical device/UNC prefix) are already removed.
bool qualifiedUnc(QStringView path) noexcept
{
    qsizetype serverEnd = 0;
    while (serverEnd < path.size() && !pathSeparator(path[serverEnd])) ++serverEnd;
    if (!serverEnd || serverEnd == path.size()) return false;
    const auto server = path.first(serverEnd);
    if (server == u"." || server == u".." || server == u"?") return false;
    const auto shareStart = serverEnd + 1;
    qsizetype shareEnd = shareStart;
    while (shareEnd < path.size() && !pathSeparator(path[shareEnd])) ++shareEnd;
    const auto share = path.sliced(shareStart, shareEnd - shareStart);
    return !share.isEmpty() && share != u"." && share != u"..";
}
bool fullyQualifiedPath(QStringView path) noexcept
{
    if (path.contains(QChar(0))) return false;
    // Preserve canonical drive/UNC namespace aliases without admitting other devices.
    if (path.size() >= 4 && path[0] == u'\\' && path[1] == u'\\'
        && (path[2] == u'?' || path[2] == u'.') && path[3] == u'\\') {
        const auto tail = path.sliced(4);
        if (tail.contains(u'/')) return false; // canonical namespace aliases use backslashes
        if (qualifiedDrive(tail)) return true;
        return tail.startsWith(u"UNC\\", Qt::CaseInsensitive) && qualifiedUnc(tail.sliced(4));
    }
    return qualifiedDrive(path) || (path.size() >= 2 && pathSeparator(path[0])
        && pathSeparator(path[1]) && qualifiedUnc(path.sliced(2)));
}
struct Timer {
    U64 &destination;
    QElapsedTimer timer;
    explicit Timer(U64 &value) : destination(value) { timer.start(); }
    ~Timer() { destination += static_cast<U64>(timer.nsecsElapsed()); }
};
struct Diagnostics {
    BuildDiagnostic callback;
    std::shared_ptr<void> context;
    std::optional<BuildError> cleanup;
    void report(const BuildError &e) noexcept { if (callback) callback(e, context.get()); }
    static void fromDensity(const Error &source, void *context) noexcept
    {
        auto &self = *static_cast<Diagnostics *>(context);
        BuildError e{BuildErrorCode::CleanupFailure, BuildPhase::Cleanup, QStringLiteral("Density cleanup failed")};
        e.density = source;
        if (!self.cleanup) self.cleanup = e;
        self.report(e);
    }
};
struct Memory {
    const BuildResourcePolicy &policy;
    BuildStatistics &stats;
    U64 fixed = 0, metadata = 0;
    void phase(U64 bytes)
    {
        budget(add(bytes, metadata, BuildPhase::Preflight), policy.workingBytes,
            BuildPhase::Preflight, "Combined working RAM budget");
        fixed = bytes; stats.workingPeak = std::max(stats.workingPeak, fixed + metadata);
    }
    void acquire(U64 bytes)
    {
        const U64 next = add(metadata, bytes, BuildPhase::Grouping);
        budget(next, policy.metadataBytes, BuildPhase::Grouping, "Occupied metadata budget");
        budget(add(fixed, next, BuildPhase::Grouping), policy.workingBytes,
            BuildPhase::Grouping, "Metadata and phase RAM exceed combined budget");
        metadata = next; stats.metadataPeak = std::max(stats.metadataPeak, metadata);
        stats.workingPeak = std::max(stats.workingPeak, fixed + metadata);
    }
    void release(U64 bytes) noexcept { metadata -= bytes; }
};
template<class T> struct Allocator {
    using value_type = T;
    Memory *memory = nullptr;
    explicit Allocator(Memory *m = nullptr) : memory(m) {}
    template<class V> Allocator(const Allocator<V> &other) noexcept : memory(other.memory) {}
    T *allocate(std::size_t n)
    {
        const U64 bytes = multiply(n, sizeof(T), BuildPhase::Grouping);
        memory->acquire(bytes);
        try { return std::allocator<T>{}.allocate(n); }
        catch (...) { memory->release(bytes); throw; }
    }
    void deallocate(T *p, std::size_t n) noexcept
    { std::allocator<T>{}.deallocate(p, n); memory->release(n * sizeof(T)); }
    template<class V> bool operator==(const Allocator<V> &other) const noexcept { return memory == other.memory; }
};
struct KeyLess {
    bool operator()(TileKey a, TileKey b) const noexcept
    { return a.y < b.y || (a.y == b.y && a.x < b.x); }
};
struct Event { TileKey group; std::uint32_t local; };
struct Group { U64 count = 0; std::uint32_t fingerprint = 0; bool exists = false; };
using Registry = std::map<TileKey, Group, KeyLess, Allocator<std::pair<const TileKey, Group>>>;

// FaultPoint is private in the entry point; the engine uses a numeric bridge.
using Hook = std::optional<BuildError> (*)(unsigned, const QString &, void *) noexcept;
using ConfigureWriter = void (*)(Writer &, Status (*)(Phase, void *) noexcept, void *);
using Transfer = std::optional<qint64> (*)(bool, QFile &, cache::MutableBytes, void *) noexcept;
struct Engine {
    const BuildRequest &r;
    BuildStatistics stats;
    BuildPhase phase = BuildPhase::Preflight;
    Memory memory;
    Registry groups;
    std::shared_ptr<Diagnostics> diagnostics;
    std::unique_ptr<QTemporaryDir> scope;
    std::unique_ptr<Writer> writer;
    std::unique_ptr<cache::CacheReader> reader;
    std::unique_ptr<Event[]> events;
    std::unique_ptr<std::byte[]> wire;
    std::unique_ptr<U64[]> counts, tile;
    U64 used = 0, liveEventBytes = 0, liveSpools = 0, payload = 0;
    U64 baseReserve = 0, scanReserve = 0, accumulationReserve = 0, finishReserve = 0;
    U64 localArea = 0;
    bool resourceCancellation = false;
    std::optional<BuildError> densityFault;
    std::optional<TileKey> currentGroup, currentTile;
    std::uint32_t reportedPhases = 0;
    BuildProgress lastProgress{};
    Hook hook;
    void *hookContext;
    ConfigureWriter configureWriter;
    Transfer transfer;
    void *transferContext;
    Engine(const BuildRequest &request, Hook h, void *hc, ConfigureWriter configure, Transfer t, void *tc)
        : r(request), memory{r.resources, stats}, groups(KeyLess{}, Allocator<std::pair<const TileKey, Group>>{&memory}),
          hook(h), hookContext(hc), configureWriter(configure), transfer(t), transferContext(tc) {}
    ~Engine() { cleanup(); }
    void progress(BuildPhase next)
    {
        phase = next;
        const auto bit = std::uint32_t{1} << static_cast<unsigned>(phase);
        const bool first = (reportedPhases & bit) == 0;
        const bool advanced = stats.scanned - lastProgress.scanned >= 4096
            || stats.eventsRead - lastProgress.eventsRead >= 16384
            || stats.visitedGroups - lastProgress.groups >= 64 || stats.tiles - lastProgress.tiles >= 64;
        if (r.progress && (first || advanced)) {
            lastProgress = {phase, stats.scanned, stats.eventsRead, stats.visitedGroups, stats.tiles};
            reportedPhases |= bit;
            r.progress(lastProgress, r.context);
        }
    }
    void check(U64 units = 1)
    {
        if (r.cancel && r.cancel(r.context)) fail(BuildErrorCode::Cancelled, phase, "Density build cancelled");
        const auto next = add(stats.workUnits, units, phase);
        budget(next, r.resources.workUnits, phase, "Combined work budget");
        stats.workUnits = next;
    }
    static bool cancel(void *context) noexcept
    {
        auto &self = *static_cast<Engine *>(context);
        if (self.r.cancel && self.r.cancel(self.r.context)) return true;
        // Shared live guard runs before CacheReader/D work, including their I/O,
        // sorting, codec and readback checkpoints. Initial mapping has a separate
        // boundary-work policy; repeated component validations are conservative.
        if (self.phase != BuildPhase::Mapping) {
            if (self.stats.workUnits == self.r.resources.workUnits) {
                self.resourceCancellation = true; return true;
            }
            ++self.stats.workUnits;
        }
        return false;
    }
    QString path(TileKey key) const
    {
        return QDir(scope->path()).filePath(QString::number(key.x) + QLatin1Char('-')
            + QString::number(key.y) + QStringLiteral(".events"));
    }
    void fault(unsigned point, const QString &name)
    {
        if (hook) { auto e = hook(point, name, hookContext); if (e) throw Failure{*e}; }
    }
    void files(U64 spools, U64 dFiles)
    {
        const U64 n = add(add(spools, dFiles, phase), 1, phase); // source retained even after close
        budget(n, r.resources.files, phase, "Simultaneously existing files budget");
        stats.filesPeak = std::max(stats.filesPeak, n);
    }
    void handles(U64 n)
    {
        budget(n, r.resources.handles, phase, "Simultaneous handles budget");
        stats.handlesPeak = std::max(stats.handlesPeak, n);
    }
    void disk(U64 eventsBytes, U64 dScratch = 0, U64 output = 0)
    {
        const U64 scratch = add(eventsBytes, dScratch, phase);
        budget(scratch, r.resources.scratchBytes, phase, "Combined event and D scratch budget");
        budget(output, r.resources.outputBytes, phase, "Output budget");
        const U64 combined = add(scratch, output, phase);
        budget(combined, r.resources.combinedDiskBytes, phase, "Combined scratch and output budget");
        stats.scratchPeak = std::max(stats.scratchPeak, scratch);
        stats.combinedDiskPeak = std::max(stats.combinedDiskPeak, combined);
    }
    [[noreturn]] void cacheFailure(const cache::Error &source, bool opening = false)
    {
        auto code = resourceCancellation ? BuildErrorCode::ResourceRefused : source.code == cache::ErrorCode::Cancelled ? BuildErrorCode::Cancelled
            : source.code == cache::ErrorCode::ResourceExhausted ? BuildErrorCode::ResourceRefused
            : opening && source.code == cache::ErrorCode::OpenFailed ? BuildErrorCode::SourceUnavailable : BuildErrorCode::CacheFailure;
        auto e = error(code, phase, "Point Cache read failed"); e.cache = source; e.offset = source.offset;
        throw Failure{std::move(e)};
    }
    [[noreturn]] void densityFailure(const Error &source)
    {
        if (densityFault) throw Failure{*densityFault};
        auto code = resourceCancellation ? BuildErrorCode::ResourceRefused : source.code == ErrorCode::Cancelled ? BuildErrorCode::Cancelled
            : source.code == ErrorCode::ResourceRefused ? BuildErrorCode::ResourceRefused
            : source.code == ErrorCode::Mapping ? BuildErrorCode::Mapping
            : source.code == ErrorCode::IO ? BuildErrorCode::OutputIO : BuildErrorCode::DensityFailure;
        auto e = error(code, phase, "Density storage failed"); e.density = source; e.tile = source.tile;
        e.required = source.required; e.budget = source.budget; throw Failure{std::move(e)};
    }
    [[noreturn]] void ioFailure(QFile &file, const char *detail, cache::Operation operation = cache::Operation::Read)
    {
        auto e = error(BuildErrorCode::ScratchIO, phase, detail);
        const auto code = operation == cache::Operation::Open ? cache::ErrorCode::OpenFailed
            : operation == cache::Operation::Write ? cache::ErrorCode::WriteFailed
            : operation == cache::Operation::Flush ? cache::ErrorCode::FlushFailed
            : operation == cache::Operation::Remove ? cache::ErrorCode::CleanupFailed : cache::ErrorCode::ReadFailed;
        e.cache = cache::makeError(code, cache::Phase::FileIO, operation, file.errorString(), {}, file.error());
        if (operation == cache::Operation::Remove) { e.code = BuildErrorCode::CleanupFailure; e.phase = BuildPhase::Cleanup; }
        if (file.pos() >= 0) e.offset = static_cast<U64>(file.pos());
        throw Failure{std::move(e)};
    }
    void open(QFile &file, QIODevice::OpenMode mode, unsigned point)
    {
        check(); fault(point, file.fileName());
        handles(writer ? 5 : 2);
        if (!file.open(mode | QIODevice::Unbuffered)) ioFailure(file, "Cannot open event spool", cache::Operation::Open);
        ++stats.eventOpens;
    }
    void remove(TileKey key, Group &group)
    {
        if (!group.exists) return;
        const auto name = path(key);
        fault(4, name);
        QFile file(name);
        if (!file.remove()) ioFailure(file, "Cannot remove event spool", cache::Operation::Remove);
        group.exists = false; --liveSpools;
        liveEventBytes -= multiply(group.count, 4, BuildPhase::Cleanup);
    }
    void cleanup() noexcept
    {
        reader.reset(); writer.reset();
        if (!scope) return;
        if (!scope->isValid()) { scope.reset(); return; }
        for (auto &[key, group] : groups) {
            if (!group.exists) continue;
            try { remove(key, group); }
            catch (const Failure &f) {
                auto e = f.error; e.code = BuildErrorCode::CleanupFailure; e.phase = BuildPhase::Cleanup;
                if (!diagnostics->cleanup) diagnostics->cleanup = e;
                diagnostics->report(e);
            }
            catch (...) {
                BuildError e{BuildErrorCode::CleanupFailure, BuildPhase::Cleanup, QStringLiteral("Cleanup allocation failed")};
                if (!diagnostics->cleanup) diagnostics->cleanup = e;
                diagnostics->report(e);
            }
        }
        try {
            if (!QDir().rmdir(scope->path())) {
                BuildError e{BuildErrorCode::CleanupFailure, BuildPhase::Cleanup, QStringLiteral("Cannot remove owned operation directory")};
                if (!diagnostics->cleanup) diagnostics->cleanup = e;
                diagnostics->report(e);
            }
        } catch (...) {
            BuildError e{BuildErrorCode::CleanupFailure, BuildPhase::Cleanup, QStringLiteral("Operation directory cleanup failed")};
            if (!diagnostics->cleanup) diagnostics->cleanup = e;
            diagnostics->report(e);
        }
        scope.reset(); // autoRemove is false: never recurse through arbitrary paths
    }
    void preflight()
    {
        progress(BuildPhase::Preflight); check();
        require(r.source.location && !r.source.cacheId.isNull() && !r.source.cachePath().empty(),
            BuildErrorCode::InvalidInput, phase, "Missing immutable source binding/identity");
        require(r.source.pointCount != 0, BuildErrorCode::EmptySource, phase, "Point Cache has no measurements");
        require(!r.outputPath.isEmpty() && !r.scratchParent.isEmpty() && QDir(r.scratchParent).exists(),
            BuildErrorCode::InvalidInput, phase, "Output path and existing scratch parent required");
        require(!QFileInfo::exists(r.outputPath), BuildErrorCode::OutputIO, phase, "Output target already exists");
        require((r.tileSide == 256 || r.tileSide == 512) && r.groupSide && r.groupSide % r.tileSide == 0,
            BuildErrorCode::InvalidInput, phase, "Group side must be a positive multiple of supported tile side");
        localArea = multiply(r.groupSide, r.groupSide, phase);
        require(localArea <= U64{UINT32_MAX} + 1, BuildErrorCode::InvalidInput, phase, "Group local indices exceed uint32");
        require(r.eventCapacity != 0, BuildErrorCode::InvalidInput, phase, "Zero event capacity");
        budget(1, r.resources.readerBytes, phase, "Reader RAM reservation required");
        size(r.eventCapacity, phase); size(multiply(r.eventCapacity, sizeof(Event), phase), phase);
        size(multiply(localArea, sizeof(U64), phase), phase);
        payload = multiply(r.source.pointCount, 4, phase);
        require(cache::checked::fileOffset(payload).has_value(), BuildErrorCode::ResourceRefused, phase, "Event payload file offset unrepresentable");
        budget(payload, r.resources.scratchBytes, phase, "Mandatory LE uint32 event payload");
        budget(payload, r.resources.combinedDiskBytes, phase, "Mandatory event payload exceeds combined disk");
        // Bounded transient QString/QFile/operation state reserve, scaled to actual paths.
        const U64 paths = add(static_cast<U64>(r.outputPath.size()), static_cast<U64>(r.scratchParent.size()), phase);
        const U64 sourcePath = static_cast<U64>(QString::fromStdWString(r.source.cachePath().wstring()).size());
        baseReserve = add(fixedBookkeeping + sizeof(Engine), multiply(add(paths, sourcePath, phase), 256, phase), phase);
        const U64 dRam = add(add(r.resources.densityBufferBytes, r.resources.densityCodecBytes, phase), r.resources.densitySortBytes, phase);
        scanReserve = add(add(baseReserve, r.resources.readerBytes, phase), add(multiply(r.eventCapacity, sizeof(Event), phase), ioBytes, phase), phase);
        accumulationReserve = add(add(baseReserve, dRam, phase), add(multiply(localArea, 8, phase), add(multiply(multiply(r.tileSide, r.tileSide, phase), 8, phase), ioBytes, phase), phase), phase);
        finishReserve = add(baseReserve, dRam, phase);
        // Check all mandatory phases before any operation-owned allocation/file.
        memory.phase(std::max({scanReserve, accumulationReserve, finishReserve}));
        handles(5); files(0, 4);
        diagnostics = std::make_shared<Diagnostics>(Diagnostics{r.diagnostic, r.diagnosticContext, {}});
        scope = std::make_unique<QTemporaryDir>(QDir(r.scratchParent).filePath(QStringLiteral(".pc2d-build-XXXXXX")));
        require(scope->isValid(), BuildErrorCode::ScratchIO, phase, "Cannot create operation scratch directory");
        scope->setAutoRemove(false);
        memory.phase(scanReserve);
    }
    void flush()
    {
        if (!used) return;
        Timer total(stats.groupingNanoseconds);
        progress(BuildPhase::Grouping); check();
        {
            Timer sorting(stats.eventSortNanoseconds);
            std::sort(events.get(), events.get() + size(used, phase), [&](const Event &a, const Event &b) {
                check(); return KeyLess{}(a.group, b.group);
            });
        }
        U64 start = 0;
        while (start < used) {
            check(); const TileKey key = events[size(start, phase)].group;
            currentGroup = key;
            U64 end = start + 1;
            while (end < used && events[size(end, phase)].group == key) { check(); ++end; }
            auto found = groups.find(key);
            if (found == groups.end()) {
                files(add(liveSpools, 1, phase), writer ? 4 : 0);
                found = groups.try_emplace(key).first;
                auto name = path(key); QFile file(name);
                open(file, QIODevice::WriteOnly | QIODevice::NewOnly, 0);
                found->second.exists = true; ++liveSpools;
            }
            auto &group = found->second;
            const U64 count = end - start;
            const U64 nextCount = add(group.count, count, phase);
            const U64 before = multiply(group.count, 4, phase), after = multiply(nextCount, 4, phase);
            require(cache::checked::fileOffset(after).has_value(), BuildErrorCode::Overflow, phase, "Event spool offset overflow");
            const U64 nextBytes = add(liveEventBytes, multiply(count, 4, phase), phase);
            disk(nextBytes);
            Timer io(stats.eventIONanoseconds);
            QFile file(path(key)); open(file, QIODevice::WriteOnly | QIODevice::Append, 1);
            require(file.size() >= 0 && static_cast<U64>(file.size()) == before,
                BuildErrorCode::InconsistentEvents, phase, "Event spool append length mismatch");
            for (U64 pos = start; pos < end;) {
                check(); const U64 n = std::min<U64>(ioBytes / 4, end - pos);
                for (U64 i = 0; i < n; ++i) cache::binary::putU32({wire.get(), size(ioBytes, phase)}, size(i * 4, phase), events[size(pos + i, phase)].local);
                const U64 bytes = n * 4;
                // Freeze the expected encoded bytes before an operation-local transfer seam.
                const auto fingerprint = crc32(group.fingerprint,
                    reinterpret_cast<const Bytef *>(wire.get()), static_cast<uInt>(bytes));
                auto overridden = transfer ? transfer(true, file, {wire.get(), size(bytes, phase)}, transferContext) : std::nullopt;
                const auto written = overridden ? *overridden
                    : file.write(reinterpret_cast<const char *>(wire.get()), static_cast<qint64>(bytes));
                if (written != static_cast<qint64>(bytes)) ioFailure(file, "Short event spool write", cache::Operation::Write);
                group.fingerprint = static_cast<std::uint32_t>(fingerprint);
                stats.eventsWritten = add(stats.eventsWritten, n, phase);
                stats.eventBytesWritten = add(stats.eventBytesWritten, bytes, phase); ++stats.eventWrites;
                pos += n;
            }
            if (!file.flush()) ioFailure(file, "Event spool flush failed", cache::Operation::Flush);
            require(file.size() >= 0 && static_cast<U64>(file.size()) == after,
                BuildErrorCode::InconsistentEvents, phase, "Event spool final append length mismatch");
            group.count = nextCount; liveEventBytes = nextBytes;
            start = end;
        }
        used = 0;
        currentGroup.reset();
        progress(BuildPhase::Scan);
    }
    void scan(const raster::Mapping &mapping)
    {
        progress(BuildPhase::SourceOpen); check();
        {
            Timer timer(stats.sourceOpenNanoseconds);
            auto opened = cache::CacheReader::open(QString::fromStdWString(r.source.cachePath().wstring()),
                {r.resources.readerBytes, 0, 1}, cancel, this);
            if (!opened) cacheFailure(opened.error(), true);
            reader = std::move(opened.value());
        }
        const auto &m = reader->metadata();
        require(m.id == r.source.cacheId && m.pointCount == r.source.pointCount
            && same({m.bounds.minX, m.bounds.minY}, r.source.minimum)
            && same({m.bounds.maxX, m.bounds.maxY}, r.source.maximum),
            BuildErrorCode::SourceMismatch, phase, "Point Cache identity/count/bounds differ from snapshot");
        events = std::make_unique<Event[]>(size(r.eventCapacity, phase));
        wire = std::make_unique<std::byte[]>(size(ioBytes, phase));
        progress(BuildPhase::Scan); check();
        std::optional<BuildError> consumerError;
        const auto openStatistics = reader->statistics();
        Timer timer(stats.scanNanoseconds); ++stats.scanAllCalls;
        auto scanned = reader->scanAll([&](std::span<const cache::PointRecord> records) -> cache::Status {
            try {
                for (const auto &record : records) {
                    check();
                    auto cell = mapping.locate(record.point);
                    if (!cell) {
                        auto e = error(BuildErrorCode::Mapping, phase, "Cannot locate source point");
                        e.mapping = cell.error(); e.recordId = record.id; e.point = record.point; throw Failure{std::move(e)};
                    }
                    auto address = buildGroupAddress(mapping.width(), mapping.height(), r.groupSide, r.tileSide, cell.value());
                    if (!address) densityFailure(address.error());
                    events[size(used, phase)] = {address.value().key, address.value().localIndex}; ++used;
                    stats.scanned = add(stats.scanned, 1, phase);
                    if (used == r.eventCapacity) flush();
                }
                return cache::success();
            } catch (const Failure &f) { consumerError = f.error; }
            catch (const std::bad_alloc &) { consumerError = error(BuildErrorCode::ResourceRefused, phase, "Allocation failed in source consumer"); }
            catch (const std::length_error &) { consumerError = error(BuildErrorCode::ResourceRefused, phase, "Consumer container size unrepresentable"); }
            return cache::makeError(cache::ErrorCode::ConsumerFailed, cache::Phase::CacheRead,
                cache::Operation::Consume, QStringLiteral("Density builder consumer failed"));
        }, cancel, this);
        stats.sourceRead = reader->statistics();
        stats.sourceRead.readBytes = add(stats.sourceRead.readBytes, openStatistics.readBytes, phase);
        stats.sourceRead.readOperations = add(stats.sourceRead.readOperations, openStatistics.readOperations, phase);
        stats.sourceRead.seekOperations = add(stats.sourceRead.seekOperations, openStatistics.seekOperations, phase);
        stats.sourceRead.indexPages = add(stats.sourceRead.indexPages, openStatistics.indexPages, phase);
        if (consumerError) throw Failure{*consumerError};
        if (!scanned) cacheFailure(scanned.error());
        flush();
        require(stats.scanned == r.source.pointCount && stats.eventsWritten == stats.scanned
            && stats.sourceRead.returnedRecords == stats.scanned && liveEventBytes == payload,
            BuildErrorCode::InconsistentEvents, phase, "Source/event count mismatch");
        stats.occupiedGroups = groups.size();
        reader.reset(); events.reset(); wire.reset(); // phase reuse only after destruction
    }
    ResourcePolicy densityPolicy()
    {
        const auto &p = r.resources;
        budget(payload, p.scratchBytes, phase, "Event scratch reservation");
        budget(payload, p.combinedDiskBytes, phase, "Combined event reservation");
        // A conservative reservation remains assigned to events until D finish.
        // Thus D cannot consume bytes already promised to still-existing spools.
        return {p.densityBufferBytes, p.densityCodecBytes, p.densitySortBytes,
            p.scratchBytes - payload, p.outputBytes, p.combinedDiskBytes - payload,
            5, p.workUnits, p.workUnits - stats.workUnits, p.mapping};
    }
    void accumulate(const raster::Mapping &mapping)
    {
        progress(BuildPhase::Accumulation); check(); memory.phase(accumulationReserve);
        files(liveSpools, 4); handles(5);
        auto created = Writer::create(r.outputPath,
            {DatasetId::create(), r.source.cacheId, r.source.pointCount, BuildContract::CompletePointCache, r.tileSide, mapping},
            densityPolicy(), scope->path(), cancel, this, Diagnostics::fromDensity, diagnostics.get(), diagnostics);
        if (!created) densityFailure(created.error());
        writer = std::move(created.value());
        configureWriter(*writer, [](Phase densityPhase, void *context) noexcept -> Status {
            auto &self = *static_cast<Engine *>(context);
            unsigned point;
            switch (densityPhase) {
            case Phase::Ingest: point = 6; break;
            case Phase::Sort: case Phase::Merge: point = 7; break;
            case Phase::Seal: case Phase::Flush: case Phase::Copy: point = 8; break;
            case Phase::Readback: case Phase::Publish: point = 9; break;
            default: return success();
            }
            if (self.hook) {
                auto failure = self.hook(point, self.r.outputPath, self.hookContext);
                if (failure) { self.densityFault = *failure; return Error{ErrorCode::Cancelled, densityPhase, QStringLiteral("Builder fault seam")}; }
            }
            return success();
        }, this);
        counts = std::make_unique<U64[]>(size(localArea, phase));
        tile = std::make_unique<U64[]>(size(multiply(r.tileSide, r.tileSide, phase), phase));
        wire = std::make_unique<std::byte[]>(size(ioBytes, phase));
        while (!groups.empty()) {
            progress(BuildPhase::Accumulation); check();
            auto it = groups.begin(); const auto key = it->first; auto &group = it->second;
            currentGroup = key;
            const U64 x0 = multiply(key.x, r.groupSide, phase), y0 = multiply(key.y, r.groupSide, phase);
            require(x0 < mapping.width() && y0 < mapping.height(), BuildErrorCode::InconsistentEvents, phase, "Group key out of bounds");
            const U64 width = std::min<U64>(r.groupSide, mapping.width() - x0);
            const U64 height = std::min<U64>(r.groupSide, mapping.height() - y0);
            {
                Timer timer(stats.accumulationNanoseconds);
                for (U64 pos = 0; pos < localArea;) {
                    check(); const U64 n = std::min<U64>(4096, localArea - pos);
                    std::fill_n(counts.get() + size(pos, phase), size(n, phase), 0); pos += n;
                }
                QFile file(path(key)); open(file, QIODevice::ReadOnly, 2);
                const U64 length = multiply(group.count, 4, phase);
                require(file.size() >= 0 && static_cast<U64>(file.size()) == length && length % 4 == 0,
                    BuildErrorCode::InconsistentEvents, phase, "Spool length/per-group count mismatch");
                U64 read = 0;
                std::uint32_t fingerprint = 0;
                while (read < group.count) {
                    check(); const U64 n = std::min<U64>(ioBytes / 4, group.count - read), bytes = n * 4;
                    {
                        Timer io(stats.eventIONanoseconds); fault(3, file.fileName());
                        auto overridden = transfer ? transfer(false, file, {wire.get(), size(bytes, phase)}, transferContext) : std::nullopt;
                        const auto got = overridden ? *overridden
                            : file.read(reinterpret_cast<char *>(wire.get()), static_cast<qint64>(bytes));
                        if (got != static_cast<qint64>(bytes)) ioFailure(file, "Short event spool read");
                        fingerprint = static_cast<std::uint32_t>(crc32(fingerprint,
                            reinterpret_cast<const Bytef *>(wire.get()), static_cast<uInt>(bytes)));
                    }
                    ++stats.eventReads; stats.eventBytesRead = add(stats.eventBytesRead, bytes, phase);
                    for (U64 i = 0; i < n; ++i) {
                        if (i % 1024 == 0) check();
                        const U64 local = *cache::binary::getU32({wire.get(), size(bytes, phase)}, size(i * 4, phase));
                        require(local < localArea && local % r.groupSide < width && local / r.groupSide < height,
                            BuildErrorCode::InconsistentEvents, phase, "Invalid local event index/partial group shape");
                        auto &value = counts[size(local, phase)];
                        auto incremented = buildCheckedCountAdd(value, 1);
                        if (!incremented) fail(BuildErrorCode::Overflow, phase, "Cell count overflow");
                        value = incremented.value();
                        stats.accumulated = add(stats.accumulated, 1, phase);
                    }
                    stats.eventsRead = add(stats.eventsRead, n, phase); read += n;
                }
                char extra;
                require(file.read(&extra, 1) == 0 && file.error() == QFileDevice::NoError,
                    BuildErrorCode::InconsistentEvents, phase, "Extra event data or spool read error");
                require(fingerprint == group.fingerprint, BuildErrorCode::InconsistentEvents,
                    phase, "Event stream fingerprint mismatch");
            }
            for (U64 y = 0; y < height; y += r.tileSide) {
                for (U64 x = 0; x < width; x += r.tileSide) {
                    progress(BuildPhase::TileCopy); check();
                    const Shape shape{static_cast<std::uint32_t>(std::min<U64>(r.tileSide, width - x)),
                        static_cast<std::uint32_t>(std::min<U64>(r.tileSide, height - y))};
                    const TileKey tileKey{(x0 + x) / r.tileSide, (y0 + y) / r.tileSide};
                    currentTile = tileKey;
                    U64 sum = 0;
                    {
                        Timer timer(stats.tileCopyNanoseconds);
                        for (U64 row = 0; row < shape.height; ++row) {
                            check(); const U64 src = (y + row) * r.groupSide + x, dst = row * shape.width;
                            for (U64 col = 0; col < shape.width; ++col) {
                                const U64 value = counts[size(src + col, phase)]; tile[size(dst + col, phase)] = value;
                                sum = add(sum, value, phase);
                            }
                        }
                    }
                    if (sum) {
                        progress(BuildPhase::DensityIngest); check();
                        Timer timer(stats.densityIngestNanoseconds);
                        auto status = writer->addTile(tileKey, shape, {tile.get(), size(U64{shape.width} * shape.height, phase)});
                        if (!status) densityFailure(status.error());
                        ++stats.tiles; stats.tileTotal = add(stats.tileTotal, sum, phase);
                        disk(liveEventBytes, writer->statistics().scratchPeak, 0);
                    }
                }
            }
            progress(BuildPhase::Accumulation); check(); remove(key, group);
            groups.erase(it); ++stats.visitedGroups;
            currentGroup.reset(); currentTile.reset();
        }
        counts.reset(); tile.reset(); wire.reset(); memory.phase(finishReserve);
        require(stats.eventsRead == stats.eventsWritten && stats.accumulated == stats.scanned
            && stats.tileTotal == stats.scanned && stats.visitedGroups == stats.occupiedGroups,
            BuildErrorCode::InconsistentEvents, phase, "Accumulation/tile/event consistency mismatch");
    }
    BuildOutput run()
    {
        preflight();
        progress(BuildPhase::Mapping); check();
        const auto mapping = [&] {
            Timer timer(stats.mappingNanoseconds);
            auto m = raster::Mapping::create(r.source.minimum, r.source.maximum, r.cell, r.resources.mapping, cancel, this);
            if (!m) {
                auto code = m.error().category() == raster::ErrorCategory::Cancelled ? BuildErrorCode::Cancelled
                    : m.error().category() == raster::ErrorCategory::ResourceRefused ? BuildErrorCode::ResourceRefused : BuildErrorCode::Mapping;
                auto e = error(code, phase, "Raster mapping validation failed"); e.mapping = m.error();
                e.required = m.error().requiredBoundaryChecks; e.budget = r.resources.mapping.maxBoundaryChecks;
                throw Failure{std::move(e)};
            }
            return m.value();
        }();
        scan(mapping); accumulate(mapping);
        progress(BuildPhase::Finish); check();
        auto completed = [&] { Timer timer(stats.finishNanoseconds); return writer->finish(); }();
        if (!completed) densityFailure(completed.error());
        auto pending = std::move(completed.value());
        stats.density = writer->statistics();
        require(stats.density.total == stats.scanned && stats.density.physicalTiles == stats.tiles,
            BuildErrorCode::InconsistentEvents, phase, "Completed D count/tile consistency mismatch");
        // Component policy permanently reserved the complete event payload. This
        // peak is a reservation bound, not a sampled sum of coincident file lengths.
        disk(payload, stats.density.scratchPeak, stats.density.outputBytes);
        writer.reset();
        progress(BuildPhase::Cleanup); cleanup();
        if (diagnostics->cleanup) throw Failure{*diagnostics->cleanup};
        progress(BuildPhase::Complete); check(); fault(5, pending.path()); check();
        return {std::move(pending), stats};
    }
};
} // namespace

BuildResourcePolicy initialBuildResourcePolicy() noexcept
{
    // Explicit initial engineering profile aligned with existing D reservations.
    // It is caller-tunable, not a geometry/production maximum or a free-space claim.
    constexpr U64 mib = 1024 * 1024, gib = 1024 * mib;
    return {32 * mib, 8 * mib, mib, 4 * gib, 4 * gib, 8 * gib, 16384, 5,
        100'000'000, raster::ValidationPolicy{raster::initialBoundaryCheckBudget}, 8 * mib, mib, mib};
}
Result<BuildGroupAddress> buildGroupAddress(U64 width, U64 height, std::uint32_t side,
    std::uint32_t tileSide, raster::CellIndex cell)
{
    if (!width || !height || !side || (tileSide != 256 && tileSide != 512) || side % tileSide
        || cell.x >= width || cell.y >= height || U64{side} * side > U64{UINT32_MAX} + 1)
        return Error{ErrorCode::InvalidInput, Phase::Metadata, QStringLiteral("Invalid construction group layout/address")};
    const TileKey key{cell.x / side, cell.y / side};
    const U64 x0 = key.x * side, y0 = key.y * side;
    const U64 local = (cell.y % side) * side + cell.x % side;
    return BuildGroupAddress{key, static_cast<std::uint32_t>(local),
        {static_cast<std::uint32_t>(std::min<U64>(side, width - x0)),
         static_cast<std::uint32_t>(std::min<U64>(side, height - y0))}};
}
BuildResult DensityBuilder::build(const BuildRequest &request) { return buildWithHook(request, nullptr, nullptr); }
BuildResult DensityBuilder::buildWithHook(const BuildRequest &request, FaultHook hook, void *context)
{ return buildWithTransferHook(request, hook, context, nullptr, nullptr); }
BuildResult DensityBuilder::buildWithTransferHook(const BuildRequest &request, FaultHook hook, void *context,
    TransferHook transfer, void *transferContext)
{
    struct Bridge { FaultHook hook; void *context; } bridge{hook, context};
    auto invoke = [](unsigned point, const QString &path, void *ctx) noexcept -> std::optional<BuildError> {
        auto &b = *static_cast<Bridge *>(ctx);
        return b.hook ? b.hook(static_cast<FaultPoint>(point), path, b.context) : std::nullopt;
    };
    std::optional<BuildRequest> snapshot;
    std::optional<Engine> engine;
    try {
        // Ancillary import provenance (notably sourcePath) is not a build input
        // and must not introduce an unbudgeted deep copy before preflight.
        snapshot.emplace(BuildRequest{
            .source = ImportedDataset{.cacheId = request.source.cacheId,
                .location = request.source.location, .pointCount = request.source.pointCount,
                .minimum = request.source.minimum, .maximum = request.source.maximum},
            .cell = request.cell, .tileSide = request.tileSide, .groupSide = request.groupSide,
            .eventCapacity = request.eventCapacity, .outputPath = request.outputPath,
            .scratchParent = request.scratchParent, .resources = request.resources,
            .cancel = request.cancel, .progress = request.progress, .context = request.context,
            .diagnostic = request.diagnostic, .diagnosticContext = request.diagnosticContext});
        // This is syntax-only: reject CWD/current-drive-dependent paths before any
        // filesystem query or user callback, including progress and cancellation.
        require(snapshot->source.location
            && fullyQualifiedPath(QString::fromStdWString(snapshot->source.cachePath().wstring()))
            && fullyQualifiedPath(snapshot->outputPath) && fullyQualifiedPath(snapshot->scratchParent),
            BuildErrorCode::InvalidInput, BuildPhase::Preflight,
            "Source, output and scratch paths must be fully qualified Windows drive or UNC paths");
        // Keep a value snapshot independent of caller mutation from callbacks.
        // The source binding itself is an immutable shared retention contract.
        auto configure = [](Writer &writer, Status (*function)(Phase, void *) noexcept, void *ctx) {
            writer.setFaultHook(function, ctx);
        };
        engine.emplace(*snapshot, +invoke, &bridge, +configure, transfer, transferContext);
        auto result = engine->run();
        engine.reset(); // Engine borrows the snapshot until its last cleanup
        return result;
    }
    catch (const Failure &failure) {
        auto e = failure.error;
        if (engine) {
            if (!e.group) e.group = engine->currentGroup;
            if (!e.tile) e.tile = engine->currentTile;
        }
        return e;
    }
    catch (const std::bad_alloc &) { return BuildError{BuildErrorCode::ResourceRefused, engine ? engine->phase : BuildPhase::Preflight, QStringLiteral("Density builder allocation failed")}; }
    catch (const std::length_error &) { return BuildError{BuildErrorCode::ResourceRefused, engine ? engine->phase : BuildPhase::Preflight, QStringLiteral("Density builder container size unrepresentable")}; }
    catch (const std::filesystem::filesystem_error &) { return BuildError{BuildErrorCode::SourceUnavailable, engine ? engine->phase : BuildPhase::Preflight, QStringLiteral("Cannot resolve physical source binding")}; }
}
Result<U64> buildCheckedCountAdd(U64 value, U64 increment)
{
    auto result = cache::checked::add(value, increment);
    if (!result) return Error{ErrorCode::InvalidInput, Phase::Ingest, QStringLiteral("Cell count uint64 overflow")};
    return *result;
}
} // namespace cadcontour::density
