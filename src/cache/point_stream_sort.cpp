#include "point_stream_sort.h"

#include <QDir>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace cadcontour::cache {
namespace {
constexpr std::uint64_t entryBytes = 40, headerBytes = 32;
constexpr std::uint64_t hashSeed = 14695981039346656037ULL;
constexpr std::uint64_t magic = 0x534e555244324350ULL; // PC2DRUNS, private scratch framing
std::uint64_t hashBytes(std::uint64_t hash, Bytes bytes)
{
    for (auto b : bytes) { hash ^= std::to_integer<unsigned>(b); hash *= 1099511628211ULL; }
    return hash;
}
std::uint64_t orderedBits(double value) noexcept
{
    if (value == 0) value = 0; // canonical key only; the stored coordinate is untouched
    auto bits = std::bit_cast<std::uint64_t>(value);
    return bits >> 63 ? ~bits : bits ^ (std::uint64_t{1} << 63);
}
template<class Entry>
bool lessEntry(const Entry &a, const Entry &b)
{
    return a.key < b.key || (a.key == b.key && a.record.id < b.record.id);
}
Error error(ErrorCode code, Phase phase, Operation operation, const char *detail)
{
    return makeError(code, phase, operation, QString::fromLatin1(detail));
}
Error resource(Phase phase)
{
    // The allocation failure path must not allocate its diagnostic string.
    return makeError(ErrorCode::ResourceExhausted, phase, Operation::Calculate,
                     QStringLiteral("Insufficient resources or unrepresentable size"));
}
Error corrupt()
{
    return error(ErrorCode::CorruptCache, Phase::Merge, Operation::Decode,
                 "Invalid temporary run framing, ordering or checksum");
}
Status inPhase(Status status, Phase phase)
{
    if (!status) { auto e = status.error(); e.phase = phase; return e; }
    return status;
}
} // namespace

Morton128 mortonKey(ModelPoint point) noexcept
{
    const auto x = orderedBits(point.x), y = orderedBits(point.y);
    Morton128 key;
    for (unsigned i = 0; i < 64; ++i) {
        auto &word = i < 32 ? key.low : key.high;
        const auto shift = 2 * (i % 32);
        word |= ((x >> i) & 1) << (shift + 1);
        word |= ((y >> i) & 1) << shift;
    }
    return key;
}

PointStreamSort::PointStreamSort(ResourceLimits limits, CancelCheck cancel, void *context)
    : limits_(limits), cancel_(cancel), cancelContext_(context)
{
}
PointStreamSort::~PointStreamSort() { cleanup(); }

Result<std::unique_ptr<PointStreamSort>> PointStreamSort::create(
    ResourceLimits limits, const QString &parent, CancelCheck cancel, void *context)
{
    try {
        auto sorter = std::unique_ptr<PointStreamSort>(new PointStreamSort(limits, cancel, context));
        if (auto s = sorter->initialize(parent); !s) return s.error();
        return sorter;
    } catch (const std::bad_alloc &) { return resource(Phase::Ingest); }
}

Status PointStreamSort::initialize(const QString &parent)
{
    static_assert(sizeof(Entry) == entryBytes);
    if (!limits_.memoryBytes || !limits_.temporaryDiskBytes || !limits_.maxOpenFiles
        || parent.isEmpty() || !QDir(parent).exists())
        return error(ErrorCode::InvalidInput, phase_, Operation::Open, "Invalid limits or temporary parent");
    // Fixed control objects, allocator headers, Qt file engines, bounded stack and path copies.
    const auto pathBytes = checked::multiply(static_cast<std::uint64_t>(QDir(parent).absolutePath().size()), 16);
    const auto fixed = pathBytes ? checked::add(65536, *pathBytes) : std::nullopt;
    if (!fixed || limits_.memoryBytes <= *fixed || !checked::memorySize(limits_.memoryBytes)
        || limits_.memoryBytes > static_cast<std::uint64_t>(PTRDIFF_MAX)) return resource(phase_);
    fixedBytes_ = *fixed;
    const auto available = limits_.memoryBytes - fixedBytes_;
    blockRecords_ = static_cast<std::size_t>(std::clamp<std::uint64_t>(available / 1024, 1, 1024));
    auto mergeBytes = [this](std::uint64_t fan) {
        return blockRecords_ * (entryBytes + sizeof(PointRecord))
            + fan * (blockRecords_ * sizeof(Entry) + sizeof(Cursor) + sizeof(std::size_t));
    };
    while (blockRecords_ > 1 && mergeBytes(2) > available) blockRecords_ /= 2;
    if (mergeBytes(2) > available || limits_.temporaryDiskBytes < headerBytes + entryBytes)
        return resource(phase_);
    fanIn_ = 2;
    while (fanIn_ < 64 && mergeBytes(fanIn_ + 1) <= available) ++fanIn_;
    const auto transferBytes = blockRecords_ * entryBytes;
    runCapacity_ = static_cast<std::size_t>((available - transferBytes) / sizeof(Entry));
    if (!runCapacity_) return resource(phase_);
    if (auto s = checkpoint(); !s) return s;
    directory_ = std::make_unique<QTemporaryDir>(QDir(parent).filePath(QStringLiteral("pc2d-sort-XXXXXX")));
    if (!directory_->isValid())
        return makeError(ErrorCode::OpenFailed, phase_, Operation::Open, directory_->errorString());
    metadata_.id = CacheId::create();
    transfer_ = std::make_unique<std::byte[]>(static_cast<std::size_t>(transferBytes));
    accountBuffers(transferBytes);
    entries_ = std::make_unique<Entry[]>(runCapacity_);
    accountBuffers(transferBytes + runCapacity_ * sizeof(Entry));
    return success();
}

void PointStreamSort::accountBuffers(std::uint64_t bytes)
{
    statistics_.liveBufferBytes = bytes;
    statistics_.peakBufferBytes = std::max(statistics_.peakBufferBytes, bytes);
}
void PointStreamSort::releaseBuffers()
{
    entries_.reset(); inputs_.reset(); transfer_.reset(); cursors_.reset(); heap_.reset(); output_.reset();
    used_ = 0;
    accountBuffers(0);
}
void PointStreamSort::close(std::unique_ptr<QFile> &file)
{
    if (file) { if (file->isOpen()) --statistics_.openFiles; file.reset(); }
}
bool PointStreamSort::cleanup()
{
    releaseBuffers();
    close(source_); close(destination_); // Destroy file engines before removing their directory.
    return !directory_ || directory_->remove();
}
Status PointStreamSort::fail(Error e)
{
    if (!firstError_) firstError_ = std::move(e);
    state_ = firstError_->code == ErrorCode::Cancelled ? State::Cancelled : State::Failed;
    cleanup(); // Secondary removal failures cannot replace the primary error.
    return *firstError_;
}
Status PointStreamSort::checkState() const
{
    if (active_) return error(ErrorCode::InvalidState, phase_, Operation::Append, "Reentrant operation");
    if (firstError_) return *firstError_;
    if (state_ != State::Accepting)
        return error(ErrorCode::InvalidState, phase_, Operation::Append, "Operation is terminal");
    return success();
}
Status PointStreamSort::checkpoint()
{
    if (cancel_ && cancel_(cancelContext_))
        return error(ErrorCode::Cancelled, phase_, Operation::Calculate, "Point sorting cancelled");
    return success();
}
Status PointStreamSort::beforeIO(Operation operation, QFile &file)
{
    return ioHook_ ? ioHook_(*this, operation, file) : success();
}
Status PointStreamSort::open(std::unique_ptr<QFile> &file, const QString &path)
{
    if (statistics_.openFiles >= limits_.maxOpenFiles) return resource(phase_);
    file = std::make_unique<QFile>(path);
    if (auto s = beforeIO(Operation::Open, *file); !s) return s;
    if (!file->open(QIODevice::ReadWrite | QIODevice::NewOnly | QIODevice::Unbuffered))
        return makeError(ErrorCode::OpenFailed, phase_, Operation::Open, file->errorString(), {}, file->error());
    ++statistics_.openFiles;
    statistics_.peakOpenFiles = std::max(statistics_.peakOpenFiles, statistics_.openFiles);
    return success();
}
Status PointStreamSort::readAt(QFile &file, std::uint64_t offset, MutableBytes bytes)
{
    if (auto s = beforeIO(Operation::Seek, file); !s) return s;
    if (auto s = file::seek(file, offset); !s) return inPhase(s, phase_);
    if (auto s = beforeIO(Operation::Read, file); !s) return s;
    return inPhase(file::readExact(file, bytes), phase_);
}
Status PointStreamSort::write(QFile &file, Bytes bytes)
{
    if (auto s = beforeIO(Operation::Write, file); !s) return s;
    return inPhase(file::writeExact(file, bytes), phase_);
}
Status PointStreamSort::flush(QFile &file)
{
    if (auto s = beforeIO(Operation::Flush, file); !s) return s;
    return inPhase(file::flush(file), phase_);
}
Status PointStreamSort::checkSize(QFile &file, std::uint64_t expected)
{
    if (auto s = beforeIO(Operation::Size, file); !s) return s;
    auto size = file::size(file);
    if (!size) return inPhase(size.error(), phase_);
    return size.value() == expected ? success() : Status{corrupt()};
}
Status PointStreamSort::diskBudget(std::uint64_t source, std::uint64_t destination)
{
    const auto total = checked::add(source, destination);
    if (!total || *total > limits_.temporaryDiskBytes || !checked::fileOffset(destination))
        return resource(phase_);
    statistics_.peakPlannedDiskBytes = std::max(statistics_.peakPlannedDiskBytes, *total);
    return success();
}

Status PointStreamSort::append(std::span<const Measurement> batch)
{
    if (auto s = checkState(); !s) return s;
    active_ = true;
    auto work = [this, batch]() -> Status {
        phase_ = Phase::Ingest;
        if (auto s = checkpoint(); !s) return s;
        if (!checked::add(metadata_.pointCount, batch.size())) return resource(phase_);
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (i % blockRecords_ == 0) { if (auto s = checkpoint(); !s) return s; }
            const auto &m = batch[i];
            if (!validPoint({m.x, m.y}) || !std::isfinite(m.z))
                return error(ErrorCode::InvalidInput, phase_, Operation::Append, "Non-finite measurement");
            entries_[used_++] = {mortonKey({m.x, m.y}), {metadata_.pointCount, {m.x, m.y}}};
            auto &b = metadata_.bounds;
            if (!metadata_.pointCount) b = {m.x, m.y, m.x, m.y};
            else {
                // fmin/fmax signed-zero results vary by implementation; select signs explicitly.
                auto min = [](double a, double v) { return v < a || (v == a && std::signbit(v)) ? v : a; };
                auto max = [](double a, double v) { return v > a || (v == a && !std::signbit(v)) ? v : a; };
                b = {min(b.minX, m.x), min(b.minY, m.y), max(b.maxX, m.x), max(b.maxY, m.y)};
            }
            ++metadata_.pointCount;
            metadata_.hasNonZeroZ |= m.z != 0;
            if (used_ == runCapacity_) {
                if (auto s = spill(); !s) return s;
                phase_ = Phase::Ingest;
            }
        }
        return checkpoint();
    };
    Status result = success();
    try { result = work(); } catch (const std::bad_alloc &) { result = resource(phase_); }
    active_ = false;
    return result ? result : fail(result.error());
}

Status PointStreamSort::finishRun(QFile &file, std::uint64_t offset, std::uint64_t count,
                                  std::uint64_t checksum)
{
    const auto length = checked::multiply(count, entryBytes);
    const auto payload = checked::add(offset, headerBytes);
    const auto end = length && payload ? checked::add(*payload, *length) : std::nullopt;
    if (!end || !checked::fileOffset(*end)) return resource(phase_);
    std::array<std::byte, headerBytes> header{};
    binary::putU64(header, 0, magic); binary::putU64(header, 8, count);
    binary::putU64(header, 16, *length); binary::putU64(header, 24, checksum);
    if (auto s = beforeIO(Operation::Seek, file); !s) return s;
    if (auto s = file::seek(file, offset); !s) return inPhase(s, phase_);
    if (auto s = write(file, header); !s) return s;
    if (auto s = beforeIO(Operation::Seek, file); !s) return s;
    return inPhase(file::seek(file, *end), phase_);
}

Status PointStreamSort::spill()
{
    if (!used_) return success();
    phase_ = Phase::RunSort;
    if (auto s = checkpoint(); !s) return s;
    std::sort(entries_.get(), entries_.get() + used_, lessEntry<Entry>);
    if (auto s = checkpoint(); !s) return s;
    phase_ = Phase::RunWrite;
    const auto payload = checked::multiply(used_, entryBytes);
    const auto length = payload ? checked::add(*payload, headerBytes) : std::nullopt;
    const auto end = length ? checked::add(generation_.bytes, *length) : std::nullopt;
    if (!end) return resource(phase_);
    if (auto s = diskBudget(0, *end); !s) return s;
    if (!source_) {
        if (auto s = open(source_, QDir(directory_->path()).filePath(QStringLiteral("a.runs"))); !s) return s;
    }
    std::array<std::byte, headerBytes> placeholder{};
    if (auto s = write(*source_, placeholder); !s) return s;
    auto hash = hashSeed;
    for (std::size_t offset = 0; offset < used_;) {
        if (auto s = checkpoint(); !s) return s;
        const auto count = std::min(blockRecords_, used_ - offset);
        MutableBytes bytes{transfer_.get(), count * entryBytes};
        for (std::size_t i = 0; i < count; ++i) {
            const auto &e = entries_[offset + i];
            auto record = bytes.subspan(i * entryBytes, entryBytes);
            binary::putU64(record, 0, e.key.high); binary::putU64(record, 8, e.key.low);
            if (auto s = encodeRecord(record.subspan(16), e.record); !s) return s;
        }
        hash = hashBytes(hash, bytes);
        if (auto s = write(*source_, bytes); !s) return s;
        offset += count;
    }
    if (auto s = finishRun(*source_, generation_.bytes, used_, hash); !s) return s;
    generation_.bytes = *end;
    ++generation_.runs;
    ++statistics_.initialRuns;
    used_ = 0;
    return checkpoint();
}

Result<PointStreamSort::Run> PointStreamSort::readRun(QFile &file, const Generation &generation,
                                                    std::uint64_t &offset)
{
    if (!checked::range(offset, headerBytes, generation.bytes)) return corrupt();
    std::array<std::byte, headerBytes> header{};
    if (auto s = readAt(file, offset, header); !s) return s.error();
    const auto count = *binary::getU64(header, 8), length = *binary::getU64(header, 16);
    const auto payload = offset + headerBytes; // range above proved addition safe
    const auto expected = checked::multiply(count, entryBytes);
    if (*binary::getU64(header, 0) != magic || count > metadata_.pointCount
        || (!count && *binary::getU64(header, 24) != hashSeed)
        || !expected || *expected != length || !checked::range(payload, length, generation.bytes))
        return corrupt();
    offset = payload + length;
    return Run{payload, count, *binary::getU64(header, 24)};
}

Status PointStreamSort::allocateMerge()
{
    entries_.reset(); // No overlap between run sorting storage and merge storage.
    accountBuffers(blockRecords_ * entryBytes);
    const auto n = fanIn_ * blockRecords_;
    inputs_ = std::make_unique<Entry[]>(n);
    accountBuffers(statistics_.liveBufferBytes + n * sizeof(Entry));
    cursors_ = std::make_unique<Cursor[]>(fanIn_);
    accountBuffers(statistics_.liveBufferBytes + fanIn_ * sizeof(Cursor));
    heap_ = std::make_unique<std::size_t[]>(fanIn_);
    accountBuffers(statistics_.liveBufferBytes + fanIn_ * sizeof(std::size_t));
    output_ = std::make_unique<PointRecord[]>(blockRecords_);
    accountBuffers(statistics_.liveBufferBytes + blockRecords_ * sizeof(PointRecord));
    return success();
}

Status PointStreamSort::refill(QFile &file, Cursor &cursor, Entry *buffer)
{
    if (auto s = checkpoint(); !s) return s;
    cursor.position = cursor.available = 0;
    const auto remaining = cursor.run.count - cursor.read;
    if (!remaining) return success();
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(blockRecords_, remaining));
    MutableBytes bytes{transfer_.get(), count * entryBytes};
    // readRun() proved the complete payload range; read never exceeds run.count.
    if (auto s = readAt(file, cursor.run.offset + cursor.read * entryBytes, bytes); !s) return s;
    cursor.checksum = hashBytes(cursor.checksum, bytes);
    for (std::size_t i = 0; i < count; ++i) {
        const auto encoded = bytes.subspan(i * entryBytes, entryBytes);
        auto decoded = decodeRecord(encoded.subspan(16));
        if (!decoded) return corrupt();
        Entry entry{{*binary::getU64(encoded, 0), *binary::getU64(encoded, 8)}, decoded.value()};
        if (entry.key != mortonKey(entry.record.point) || entry.record.id >= metadata_.pointCount
            || (cursor.hasPrevious && !lessEntry(cursor.previous, entry))) return corrupt();
        buffer[i] = entry;
        cursor.previous = entry;
        cursor.hasPrevious = true;
    }
    cursor.read += count;
    cursor.available = count;
    if (cursor.read == cursor.run.count && cursor.checksum != cursor.run.checksum) return corrupt();
    return success();
}

Status PointStreamSort::mergeGroup(QFile &source, std::size_t count, QFile *destination, const Sink *sink)
{
    std::size_t heapSize = 0, outputCount = 0;
    std::uint64_t total = 0, emitted = 0, hash = hashSeed, start = 0;
    if (destination) {
        const auto pos = destination->pos();
        if (pos < 0) return resource(phase_);
        start = static_cast<std::uint64_t>(pos);
        std::array<std::byte, headerBytes> placeholder{};
        if (auto s = write(*destination, placeholder); !s) return s;
    }
    auto greater = [this](std::size_t a, std::size_t b) {
        return lessEntry(inputs_[b * blockRecords_ + cursors_[b].position],
                         inputs_[a * blockRecords_ + cursors_[a].position]);
    };
    for (std::size_t i = 0; i < count; ++i) {
        auto &c = cursors_[i];
        const auto sum = checked::add(total, c.run.count);
        if (!sum || *sum > metadata_.pointCount) return corrupt();
        total = *sum;
        if (auto s = refill(source, c, inputs_.get() + i * blockRecords_); !s) return s;
        if (c.available) { heap_[heapSize++] = i; std::push_heap(heap_.get(), heap_.get() + heapSize, greater); }
    }
    auto flushOutput = [&]() -> Status {
        if (auto s = checkpoint(); !s) return s;
        Status result = success();
        if (destination) {
            Bytes bytes{transfer_.get(), outputCount * entryBytes};
            hash = hashBytes(hash, bytes);
            result = write(*destination, bytes);
        } else {
            try { result = (*sink)({output_.get(), outputCount}); }
            catch (...) { result = error(ErrorCode::ConsumerFailed, phase_, Operation::Consume, "Sink threw an exception"); }
        }
        outputCount = 0;
        if (!result) return result;
        return checkpoint();
    };
    Entry previous{};
    bool hasPrevious = false;
    while (heapSize) {
        std::pop_heap(heap_.get(), heap_.get() + heapSize, greater);
        const auto index = heap_[--heapSize];
        auto &c = cursors_[index];
        const auto entry = inputs_[index * blockRecords_ + c.position++];
        if (hasPrevious && !lessEntry(previous, entry)) return corrupt();
        previous = entry; hasPrevious = true;
        if (destination) {
            MutableBytes bytes{transfer_.get() + outputCount * entryBytes, entryBytes};
            binary::putU64(bytes, 0, entry.key.high); binary::putU64(bytes, 8, entry.key.low);
            if (auto s = encodeRecord(bytes.subspan(16), entry.record); !s) return s;
        } else output_[outputCount] = entry.record;
        ++outputCount; ++emitted;
        // transfer_ is shared with refill, so flush staged output before any refill.
        if (outputCount == blockRecords_ || c.position == c.available) {
            if (auto s = flushOutput(); !s) return s;
        }
        if (c.position == c.available) {
            if (auto s = refill(source, c, inputs_.get() + index * blockRecords_); !s) return s;
        }
        if (c.position != c.available) {
            heap_[heapSize++] = index;
            std::push_heap(heap_.get(), heap_.get() + heapSize, greater);
        }
    }
    if (outputCount) { if (auto s = flushOutput(); !s) return s; }
    if (emitted != total) return corrupt();
    return destination ? finishRun(*destination, start, total, hash) : success();
}

Status PointStreamSort::verifyGeneration(QFile &file, const Generation &generation)
{
    if (auto s = checkSize(file, generation.bytes); !s) return s;
    std::uint64_t offset = 0, total = 0;
    for (std::uint64_t i = 0; i < generation.runs; ++i) {
        if (auto s = checkpoint(); !s) return s;
        auto run = readRun(file, generation, offset);
        if (!run) return run.error();
        const auto sum = checked::add(total, run.value().count);
        if (!sum || *sum > metadata_.pointCount) return corrupt();
        total = *sum;
        Cursor cursor{}; cursor.run = run.value(); cursor.checksum = hashSeed;
        while (cursor.read != cursor.run.count) {
            if (auto s = refill(file, cursor, inputs_.get()); !s) return s;
        }
    }
    return offset == generation.bytes && total == metadata_.pointCount ? success() : Status{corrupt()};
}

Status PointStreamSort::consume(const Sink &sink)
{
    if (auto s = spill(); !s) return s;
    if (!generation_.runs) { phase_ = Phase::Output; return checkpoint(); }
    if (auto s = flush(*source_); !s) return s;
    if (auto s = checkSize(*source_, generation_.bytes); !s) return s;
    if (auto s = allocateMerge(); !s) return s;
    while (generation_.runs > fanIn_) {
        phase_ = Phase::Merge;
        if (auto s = checkpoint(); !s) return s;
        if (limits_.maxOpenFiles < 2) return resource(phase_);
        Generation next;
        next.runs = *checked::pageCount(generation_.runs, fanIn_);
        const auto payload = checked::multiply(metadata_.pointCount, entryBytes);
        const auto headers = checked::multiply(next.runs, headerBytes);
        const auto bytes = payload && headers ? checked::add(*payload, *headers) : std::nullopt;
        if (!bytes) return resource(phase_);
        next.bytes = *bytes;
        if (auto s = diskBudget(generation_.bytes, next.bytes); !s) return s;
        const auto name = alternate_ ? QStringLiteral("a.runs") : QStringLiteral("b.runs");
        if (auto s = open(destination_, QDir(directory_->path()).filePath(name)); !s) return s;
        std::uint64_t offset = 0;
        for (std::uint64_t first = 0; first < generation_.runs;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(fanIn_, generation_.runs - first));
            for (std::size_t i = 0; i < count; ++i) {
                auto run = readRun(*source_, generation_, offset);
                if (!run) return run.error();
                cursors_[i] = {}; cursors_[i].run = run.value(); cursors_[i].checksum = hashSeed;
            }
            if (auto s = mergeGroup(*source_, count, destination_.get(), nullptr); !s) return s;
            first += count;
        }
        if (offset != generation_.bytes) return corrupt();
        if (auto s = flush(*destination_); !s) return s;
        if (auto s = verifyGeneration(*destination_, next); !s) return s;
        if (auto s = checkpoint(); !s) return s;
        const auto oldPath = source_->fileName();
        close(source_);
        if (!QFile::remove(oldPath))
            return error(ErrorCode::CleanupFailed, phase_, Operation::Remove, "Cannot remove previous generation");
        source_ = std::move(destination_);
        generation_ = next;
        alternate_ = !alternate_;
        ++statistics_.mergeGenerations;
    }
    phase_ = Phase::Output;
    if (auto s = checkpoint(); !s) return s;
    std::uint64_t offset = 0, total = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(generation_.runs); ++i) {
        auto run = readRun(*source_, generation_, offset);
        if (!run) return run.error();
        const auto sum = checked::add(total, run.value().count);
        if (!sum || *sum > metadata_.pointCount) return corrupt();
        total = *sum;
        cursors_[i] = {}; cursors_[i].run = run.value(); cursors_[i].checksum = hashSeed;
    }
    if (offset != generation_.bytes || total != metadata_.pointCount) return corrupt();
    return mergeGroup(*source_, static_cast<std::size_t>(generation_.runs), nullptr, &sink);
}

Result<CacheMetadata> PointStreamSort::finish(const Sink &sink)
{
    if (auto s = checkState(); !s) return s.error();
    active_ = true;
    state_ = State::Consuming;
    Status result = success();
    try {
        if (!sink) result = error(ErrorCode::InvalidInput, phase_, Operation::Consume, "Missing output sink");
        else result = consume(sink);
        if (result) { phase_ = Phase::Output; result = checkpoint(); }
    } catch (const std::bad_alloc &) { result = resource(phase_); }
    active_ = false;
    if (!result) return fail(result.error()).error();
    phase_ = Phase::Cleanup;
    if (!cleanup())
        return fail(error(ErrorCode::CleanupFailed, phase_, Operation::Remove, "Cannot remove temporary directory")).error();
    state_ = State::Succeeded;
    return metadata_;
}

} // namespace cadcontour::cache
