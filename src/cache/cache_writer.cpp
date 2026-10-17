#include "cache_writer.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <new>

namespace cadcontour::cache {
namespace {
constexpr std::uint64_t descriptorSize = format::indexEntrySize + format::checksumSize;
Error resource(Phase phase)
{
    return makeError(ErrorCode::ResourceExhausted, phase, Operation::Calculate,
                     QStringLiteral("Insufficient cache writer resources or unrepresentable size"));
}
Error corrupt(Phase phase)
{
    return makeError(ErrorCode::CorruptCache, phase, Operation::Decode,
                     QStringLiteral("Inconsistent cache pages, descriptors or metadata"));
}
double minimum(double a, double b) { return b < a || (b == a && std::signbit(b)) ? b : a; }
double maximum(double a, double b) { return b > a || (b == a && !std::signbit(b)) ? b : a; }
BoundingBox unite(BoundingBox a, BoundingBox b)
{
    return {minimum(a.minX, b.minX), minimum(a.minY, b.minY),
            maximum(a.maxX, b.maxX), maximum(a.maxY, b.maxY)};
}
bool equal(BoundingBox a, BoundingBox b)
{
    auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
    return bits(a.minX) == bits(b.minX) && bits(a.minY) == bits(b.minY)
        && bits(a.maxX) == bits(b.maxX) && bits(a.maxY) == bits(b.maxY);
}
bool equal(const IndexEntry &a, const IndexEntry &b)
{
    return a.childPage == b.childPage && a.childKind == b.childKind
        && a.pointCount == b.pointCount && equal(a.bounds, b.bounds);
}
Result<IndexEntry> aggregate(std::span<const IndexEntry> entries, std::uint64_t page,
                             Phase phase)
{
    if (entries.empty()) return corrupt(phase);
    IndexEntry result{entries.front().bounds, page, 0, PageKind::Index};
    for (const auto &entry : entries) {
        auto sum = checked::add(result.pointCount, entry.pointCount);
        if (!sum) return resource(phase);
        result.pointCount = *sum;
        result.bounds = unite(result.bounds, entry.bounds);
    }
    return result;
}
Result<std::uint64_t> pageOffset(std::uint64_t start, std::uint64_t ordinal)
{
    auto length = checked::multiply(ordinal, format::pageSize);
    auto offset = length ? checked::add(start, *length) : std::nullopt;
    if (!offset || !checked::fileOffset(*offset)) return resource(Phase::Layout);
    return *offset;
}
} // namespace

CacheWriter::CacheWriter(ResourceLimits limits, CancelCheck cancel, void *context)
    : limits_(limits), cancel_(cancel), cancelContext_(context) {}
CacheWriter::~CacheWriter() { cleanup(); }

Result<std::unique_ptr<CacheWriter>> CacheWriter::create(
    const QString &path, ResourceLimits limits, const QString &parent, CancelCheck cancel, void *context)
{
    try {
        auto writer = std::unique_ptr<CacheWriter>(new CacheWriter(limits, cancel, context));
        if (auto s = writer->initialize(path, parent); !s) return s.error();
        return writer;
    } catch (const std::bad_alloc &) { return resource(Phase::Ingest); }
}

Status CacheWriter::initialize(const QString &path, const QString &parent)
{
    if (!limits_.memoryBytes || !limits_.temporaryDiskBytes || !limits_.maxOpenFiles)
        return makeError(ErrorCode::InvalidInput, phase_, Operation::Open, QStringLiteral("Zero resource budget"));
    // One encoded page, one phase-specific array, fixed control/stack/Qt reserve and paths.
    const auto pathLength = checked::add(static_cast<std::uint64_t>(QFileInfo(path).absoluteFilePath().size()),
                                         static_cast<std::uint64_t>(QDir(parent).absolutePath().size()));
    const auto paths = pathLength ? checked::multiply(*pathLength, 16) : std::nullopt;
    constexpr auto arrays = std::max(format::pointCapacity * sizeof(PointRecord),
                                      format::indexCapacity * sizeof(IndexEntry));
    const auto reserve = paths ? checked::add(65536 + format::pageSize + arrays, *paths) : std::nullopt;
    if (!reserve || limits_.memoryBytes <= *reserve || limits_.maxOpenFiles < 4)
        return resource(phase_);
    writerReserve_ = *reserve;
    writerDiskLimit_ = limits_.temporaryDiskBytes / 2;
    if (writerDiskLimit_ < format::headerSize + format::footerSize) return resource(phase_);
    auto sorted = PointStreamSort::create({limits_.memoryBytes - writerReserve_,
        limits_.temporaryDiskBytes - writerDiskLimit_, limits_.maxOpenFiles - 2}, parent, cancel_, cancelContext_);
    if (!sorted) return sorted.error();
    sorter_ = std::move(sorted.value());
    if (auto s = checkpoint(); !s) return s;
    auto staged = StagedFile::create(path);
    if (!staged) return staged.error();
    staged_ = std::move(staged.value());
    directory_ = std::make_unique<QTemporaryDir>(QDir(parent).filePath(QStringLiteral("pc2d-writer-XXXXXX")));
    if (!directory_->isValid())
        return makeError(ErrorCode::OpenFailed, phase_, Operation::Open, directory_->errorString());
    descriptors_ = std::make_unique<QFile>(QDir(directory_->path()).filePath(QStringLiteral("points.desc")));
    if (!descriptors_->open(QIODevice::ReadWrite | QIODevice::NewOnly | QIODevice::Unbuffered))
        return makeError(ErrorCode::OpenFailed, phase_, Operation::Open, descriptors_->errorString(), {}, descriptors_->error());
    page_ = std::make_unique<std::byte[]>(format::pageSize);
    records_ = std::make_unique<PointRecord[]>(format::pointCapacity);
    return writeCache(0, {page_.get(), format::headerSize}); // deliberately incomplete until sealing
}

Status CacheWriter::available() const
{
    if (active_) return makeError(ErrorCode::InvalidState, phase_, Operation::Append, QStringLiteral("Reentrant cache writer"));
    if (firstError_) return *firstError_;
    if (state_ != State::Accepting)
        return makeError(ErrorCode::InvalidState, phase_, Operation::Append, QStringLiteral("Cache writer is terminal"));
    return success();
}
Status CacheWriter::checkpoint()
{
    if (cancel_ && cancel_(cancelContext_))
        return makeError(ErrorCode::Cancelled, phase_, Operation::Calculate, QStringLiteral("Cache writing cancelled"));
    return success();
}
bool CacheWriter::cleanup()
{
    const bool sortedClean = !sorter_ || sorter_->cleanup();
    if (sortedClean) sorter_.reset(); // retain a failed directory removal for destructor retry
    staged_.reset(); descriptors_.reset();
    page_.reset(); records_.reset(); entries_.reset();
    const bool descriptorsClean = !directory_ || directory_->remove();
    return sortedClean && descriptorsClean;
}
Status CacheWriter::fail(Error error)
{
    if (!firstError_) firstError_ = std::move(error);
    state_ = firstError_->code == ErrorCode::Cancelled ? State::Cancelled : State::Failed;
    cleanup();
    return *firstError_;
}
Status CacheWriter::beforeIO(Operation operation)
{
    return ioHook_ ? ioHook_(*this, operation) : success();
}
Status CacheWriter::diskBudget(std::uint64_t cacheBytes, std::uint64_t descriptorBytes)
{
    auto sum = checked::add(cacheBytes, descriptorBytes);
    if (!sum || *sum > writerDiskLimit_ || !checked::fileOffset(cacheBytes)
        || !checked::fileOffset(descriptorBytes)) return resource(phase_);
    peakDiskBytes_ = std::max(peakDiskBytes_, *sum);
    return success();
}
Status CacheWriter::writeCache(std::uint64_t offset, Bytes bytes)
{
    const auto end = checked::add(offset, bytes.size());
    if (!end) return resource(phase_);
    if (auto s = diskBudget(std::max(cacheBytes_, *end), descriptorBytes_); !s) return s;
    if (auto s = beforeIO(Operation::Seek); !s) return s;
    if (auto s = staged_->seek(offset); !s) return s;
    if (auto s = beforeIO(Operation::Write); !s) return s;
    if (auto s = staged_->write(bytes); !s) return s;
    cacheBytes_ = std::max(cacheBytes_, *end);
    return success();
}
Status CacheWriter::readAt(QIODevice &device, std::uint64_t offset, MutableBytes bytes)
{
    if (auto s = beforeIO(Operation::Seek); !s) return s;
    if (auto s = file::seek(device, offset); !s) return s;
    if (auto s = beforeIO(Operation::Read); !s) return s;
    return file::readExact(device, bytes);
}
Status CacheWriter::writeDescriptor(const IndexEntry &entry)
{
    const auto end = checked::add(descriptorBytes_, descriptorSize);
    if (!end) return resource(phase_);
    if (auto s = diskBudget(cacheBytes_, *end); !s) return s;
    std::array<std::byte, descriptorSize> bytes{};
    if (auto s = encodeIndexEntry(MutableBytes(bytes).first(format::indexEntrySize), entry); !s) return s;
    if (auto s = sealChecksum(bytes); !s) return s;
    if (auto s = beforeIO(Operation::Write); !s) return s;
    if (auto s = file::writeExact(*descriptors_, bytes); !s) return s;
    descriptorBytes_ = *end;
    return success();
}
Result<IndexEntry> CacheWriter::readDescriptor(std::uint64_t ordinal)
{
    const auto offset = checked::multiply(ordinal, descriptorSize);
    if (!offset || !checked::range(*offset, descriptorSize, descriptorBytes_)) return corrupt(phase_);
    std::array<std::byte, descriptorSize> bytes{};
    if (auto s = readAt(*descriptors_, *offset, bytes); !s) return s.error();
    if (auto s = verifyChecksum(bytes); !s) return s.error();
    return decodeIndexEntry(Bytes(bytes).first(format::indexEntrySize));
}

Status CacheWriter::append(std::span<const Measurement> batch)
{
    if (auto s = available(); !s) return s;
    active_ = true;
    Status result = success();
    try {
        result = checkpoint();
        if (result) result = sorter_->append(batch);
    } catch (const std::bad_alloc &) { result = resource(phase_); }
    active_ = false;
    return result ? result : fail(result.error());
}
Status CacheWriter::consume(std::span<const PointRecord> records)
{
    phase_ = Phase::PointPages;
    for (const auto &record : records) {
        if (!validPoint(record.point)) return corrupt(phase_);
        const auto count = checked::add(streamCount_, 1);
        if (!count) return resource(phase_);
        const BoundingBox b{record.point.x, record.point.y, record.point.x, record.point.y};
        streamBounds_ = streamCount_ ? unite(streamBounds_, b) : b;
        streamCount_ = *count;
        records_[used_++] = record;
        if (used_ == format::pointCapacity) {
            if (auto s = finishPointPage(); !s) return s;
        }
    }
    return checkpoint();
}
Status CacheWriter::finishPointPage()
{
    if (!used_) return success();
    if (auto s = checkpoint(); !s) return s;
    MutableBytes page{page_.get(), format::pageSize};
    if (auto s = encodePointPage(page, pointPages_, {records_.get(), used_}); !s) return s;
    auto offset = pageOffset(format::headerSize, pointPages_);
    if (!offset) return offset.error();
    if (auto s = writeCache(offset.value(), page); !s) return s;
    auto p = records_[0].point;
    BoundingBox bounds{p.x, p.y, p.x, p.y};
    for (std::size_t i = 1; i < used_; ++i) {
        p = records_[i].point;
        bounds = unite(bounds, {p.x, p.y, p.x, p.y});
    }
    if (auto s = writeDescriptor({bounds, pointPages_, used_, PageKind::Points}); !s) return s;
    const auto next = checked::add(pointPages_, 1);
    if (!next) return resource(phase_);
    pointPages_ = *next;
    used_ = 0;
    return success();
}

Result<IndexEntry> CacheWriter::summarize(Bytes page, std::uint64_t ordinal, std::uint32_t level)
{
    auto h = validatePage(page);
    if (!h) return h.error();
    if (h.value().kind != PageKind::Index || h.value().pageId != ordinal || h.value().level != level)
        return corrupt(phase_);
    IndexEntry summary{{}, ordinal, 0, PageKind::Index};
    for (std::size_t i = 0; i < h.value().count; ++i) {
        auto entry = decodeIndexEntry(page.subspan(format::pageHeaderSize + i * format::indexEntrySize,
                                                  format::indexEntrySize));
        if (!entry) return entry.error();
        const auto sum = checked::add(summary.pointCount, entry.value().pointCount);
        if (!sum) return resource(phase_);
        summary.bounds = i ? unite(summary.bounds, entry.value().bounds) : entry.value().bounds;
        summary.pointCount = *sum;
    }
    return summary;
}

Status CacheWriter::buildIndex()
{
    phase_ = Phase::IndexBuild;
    if (auto s = checkpoint(); !s) return s;
    records_.reset(); // sorter and point array no longer live in the index phase
    entries_ = std::make_unique<IndexEntry[]>(format::indexCapacity);
    if (auto s = beforeIO(Operation::Flush); !s) return s;
    if (auto s = file::flush(*descriptors_); !s) return s;
    auto size = file::size(*descriptors_);
    if (!size) return size.error();
    if (size.value() != descriptorBytes_) return corrupt(phase_);
    std::uint64_t sourceStart = 0, sourceCount = pointPages_, nextPage = 0;
    std::uint32_t level = 1;
    MutableBytes page{page_.get(), format::pageSize};
    while (sourceCount) {
        const auto levelStart = nextPage;
        for (std::uint64_t first = 0; first < sourceCount;) {
            if (auto s = checkpoint(); !s) return s;
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(format::indexCapacity, sourceCount - first));
            for (std::size_t i = 0; i < count; ++i) {
                if (auto s = checkpoint(); !s) return s;
                Result<IndexEntry> child = corrupt(phase_);
                if (level == 1) child = readDescriptor(first + i);
                else {
                    auto offset = pageOffset(layout_.indexOffset, sourceStart + first + i);
                    if (!offset) return offset.error();
                    if (auto s = beforeIO(Operation::Read); !s) return s;
                    if (auto s = staged_->seek(offset.value()); !s) return s;
                    if (auto s = staged_->read(page); !s) return s;
                    child = summarize(page, sourceStart + first + i, level - 1);
                }
                if (!child) return child.error();
                if (level == 1 && (child.value().childPage != first + i
                    || child.value().childKind != PageKind::Points)) return corrupt(phase_);
                entries_[i] = child.value();
            }
            auto summary = aggregate({entries_.get(), count}, nextPage, phase_);
            if (!summary) return summary.error();
            if (auto s = encodeIndexPage(page, nextPage, level, {entries_.get(), count}); !s) return s;
            auto offset = pageOffset(layout_.indexOffset, nextPage);
            if (!offset) return offset.error();
            if (auto s = writeCache(offset.value(), page); !s) return s;
            auto next = checked::add(nextPage, 1);
            if (!next) return resource(phase_);
            nextPage = *next;
            first += count; // bounded by sourceCount
            if (sourceCount <= format::indexCapacity) {
                if (summary.value().pointCount != header_.metadata.pointCount
                    || !equal(summary.value().bounds, header_.metadata.bounds)) return corrupt(phase_);
                header_.index = {IndexKind::BoundingBoxTree, nextPage, nextPage - 1, level};
                return success();
            }
        }
        sourceStart = levelStart;
        sourceCount = nextPage - levelStart;
        if (level == UINT32_MAX) return resource(phase_);
        ++level;
    }
    return success();
}

Status CacheWriter::validate(QIODevice &device, std::uint64_t expected)
{
    MutableBytes page{page_.get(), format::pageSize};
    if (auto s = checkpoint(); !s) return s;
    if (auto s = readAt(device, 0, page.first(format::headerSize)); !s) return s;
    auto h = decodeHeader(page.first(format::headerSize));
    if (!h) return h.error();
    if (auto s = readAt(device, layout_.footerOffset, page.first(format::footerSize)); !s) return s;
    auto f = decodeFooter(page.first(format::footerSize));
    if (!f) return f.error();
    if (auto s = validateEnvelope(h.value(), f.value(), expected); !s) return s;
    const auto &m = h.value().metadata;
    const auto &index = h.value().index;
    if (m.id != header_.metadata.id || m.pointCount != streamCount_
        || m.hasNonZeroZ != header_.metadata.hasNonZeroZ || !equal(m.bounds, streamBounds_)
        || index.kind != header_.index.kind || index.pageCount != header_.index.pageCount
        || index.rootPage != header_.index.rootPage || index.height != header_.index.height) return corrupt(phase_);
    std::uint64_t childStart = 0, children = pointPages_, ordinal = 0;
    std::uint32_t level = 1;
    while (children) {
        const auto levelStart = ordinal;
        const auto parents = *checked::pageCount(children, format::indexCapacity);
        std::uint64_t consumed = 0;
        for (std::uint64_t p = 0; p < parents; ++p) {
            if (auto s = checkpoint(); !s) return s;
            auto offset = pageOffset(layout_.indexOffset, ordinal);
            if (!offset) return offset.error();
            if (auto s = readAt(device, offset.value(), page); !s) return s;
            auto ph = validatePage(page);
            if (!ph) return ph.error();
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(format::indexCapacity, children - consumed));
            if (ph.value().kind != PageKind::Index || ph.value().pageId != ordinal
                || ph.value().level != level || ph.value().count != count) return corrupt(phase_);
            for (std::size_t i = 0; i < count; ++i) {
                auto e = decodeIndexEntry(Bytes(page).subspan(format::pageHeaderSize + i * format::indexEntrySize,
                                                             format::indexEntrySize));
                if (!e) return e.error();
                entries_[i] = e.value();
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (auto s = checkpoint(); !s) return s;
                const auto child = childStart + consumed + i;
                Result<IndexEntry> expectedEntry = corrupt(phase_);
                if (level == 1) {
                    expectedEntry = readDescriptor(child);
                    const auto recordsBefore = checked::multiply(child, format::pointCapacity);
                    if (!recordsBefore || *recordsBefore >= m.pointCount) return corrupt(phase_);
                    const auto points = std::min<std::uint64_t>(format::pointCapacity, m.pointCount - *recordsBefore);
                    if (entries_[i].childPage != child || entries_[i].childKind != PageKind::Points
                        || entries_[i].pointCount != points)
                        return corrupt(phase_);
                } else {
                    offset = pageOffset(layout_.indexOffset, child);
                    if (!offset) return offset.error();
                    if (auto s = readAt(device, offset.value(), page); !s) return s;
                    expectedEntry = summarize(page, child, level - 1);
                }
                if (!expectedEntry) return expectedEntry.error();
                if (!equal(entries_[i], expectedEntry.value())) return corrupt(phase_);
            }
            auto summary = aggregate({entries_.get(), count}, ordinal, phase_);
            if (!summary) return summary.error();
            if (parents == 1 && (summary.value().pointCount != m.pointCount || !equal(summary.value().bounds, m.bounds)
                || ordinal != index.rootPage || level != index.height)) return corrupt(phase_);
            const auto next = checked::add(ordinal, 1);
            if (!next) return resource(phase_);
            ordinal = *next;
            consumed += count;
        }
        if (parents == 1) break;
        childStart = levelStart;
        children = parents;
        if (level == UINT32_MAX) return resource(phase_);
        ++level;
    }
    if (ordinal != index.pageCount) return corrupt(phase_);
    // Cancellation and scratch cleanup are inside the publication gate, before rename.
    if (auto s = checkpoint(); !s) return s;
    descriptors_.reset();
    if (!directory_->remove())
        return makeError(ErrorCode::CleanupFailed, Phase::Cleanup, Operation::Remove,
                         QStringLiteral("Cannot remove cache descriptor directory"));
    return checkpoint();
}

Result<PendingCache> CacheWriter::seal()
{
    phase_ = Phase::PointPages;
    auto sorted = sorter_->finish([this](auto records) -> Status {
        try { return consume(records); }
        catch (const std::bad_alloc &) { return resource(phase_); }
    });
    if (!sorted) return sorted.error(); // even a fully emitted stream is still provisional
    header_.metadata = sorted.value();
    sorter_.reset();
    if (auto s = finishPointPage(); !s) return s.error();
    if (streamCount_ != header_.metadata.pointCount || !equal(streamBounds_, header_.metadata.bounds))
        return corrupt(phase_);
    auto layout = calculateLayout(streamCount_, 0);
    if (!layout) return layout.error();
    layout_ = layout.value();
    if (layout_.pointPages != pointPages_) return corrupt(phase_);
    if (auto s = buildIndex(); !s) return s.error();
    phase_ = Phase::Sealing;
    if (auto s = checkpoint(); !s) return s.error();
    layout = calculateLayout(streamCount_, header_.index.pageCount);
    if (!layout) return layout.error();
    layout_ = layout.value();
    MutableBytes page{page_.get(), format::pageSize};
    if (auto s = encodeHeader(page.first(format::headerSize), header_); !s) return s.error();
    if (auto s = writeCache(0, page.first(format::headerSize)); !s) return s.error();
    CompletionFooter footer{header_.metadata.id, streamCount_, layout_.fileSize, pointPages_,
                            header_.index.pageCount, layout_.footerOffset};
    if (auto s = encodeFooter(page.first(format::footerSize), footer); !s) return s.error();
    if (auto s = writeCache(layout_.footerOffset, page.first(format::footerSize)); !s) return s.error();
    if (auto s = beforeIO(Operation::Publish); !s) return s.error();
    if (auto s = checkpoint(); !s) return s.error();
    auto validator = [](QIODevice &device, std::uint64_t size, void *context) -> Status {
        auto &writer = *static_cast<CacheWriter *>(context);
        try { return writer.validate(device, size); }
        catch (const std::bad_alloc &) { return resource(writer.phase_); }
    };
    auto published = staged_->publish(layout_.fileSize, validator, this);
    if (!published) return published.error();
    return PendingCache{std::move(published.value()), header_, layout_};
}
Result<PendingCache> CacheWriter::finish()
{
    if (auto s = available(); !s) return s.error();
    active_ = true;
    state_ = State::Finishing;
    auto work = [this]() -> Result<PendingCache> {
        try { return seal(); }
        catch (const std::bad_alloc &) { return resource(phase_); }
    };
    auto result = work();
    active_ = false;
    if (!result) return fail(result.error()).error();
    state_ = State::Succeeded;
    cleanup();
    return result;
}

} // namespace cadcontour::cache
