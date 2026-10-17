#include "cache_reader.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>

namespace cadcontour::cache {
namespace {
Error corrupt(Phase phase)
{
    return makeError(ErrorCode::CorruptCache, phase, Operation::Decode,
                     QStringLiteral("Inconsistent packed cache hierarchy, records or bounds"));
}
Error resource(Phase phase)
{
    return makeError(ErrorCode::ResourceExhausted, phase, Operation::Calculate,
                     QStringLiteral("Insufficient cache reader resources"));
}
Error busy()
{
    return makeError(ErrorCode::InvalidState, Phase::CacheRead, Operation::Read,
                     QStringLiteral("Cache reader already has an active operation"));
}
struct ActiveGuard {
    std::atomic<bool> &active;
    ~ActiveGuard() { active.store(false, std::memory_order_release); }
};
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
bool contains(BoundingBox a, BoundingBox b)
{
    return a.minX <= b.minX && a.minY <= b.minY && a.maxX >= b.maxX && a.maxY >= b.maxY;
}
bool intersects(BoundingBox a, BoundingBox b)
{
    return a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY;
}
bool matches(std::span<const BoundingBox> boxes, BoundingBox bounds)
{
    return std::any_of(boxes.begin(), boxes.end(), [bounds](auto b) { return intersects(b, bounds); });
}
} // namespace

Result<std::unique_ptr<CacheReader>> CacheReader::open(
    const QString &path, ResourceLimits limits, CancelCheck cancel, void *context)
{
    try {
        auto reader = std::unique_ptr<CacheReader>(new CacheReader(path));
        reader->cancel_ = cancel;
        reader->cancelContext_ = context;
        if (auto s = reader->initialize(limits); !s) return s.error();
        reader->cancel_ = nullptr;
        reader->cancelContext_ = nullptr;
        return reader;
    } catch (const std::bad_alloc &) { return resource(Phase::CacheRead); }
}

Status CacheReader::initialize(ResourceLimits limits)
{
    if (!limits.memoryBytes || !limits.maxOpenFiles)
        return makeError(ErrorCode::InvalidInput, phase_, Operation::Open, QStringLiteral("Zero reader budget"));
    const auto pathBytes = checked::multiply(static_cast<std::uint64_t>(QFileInfo(file_).absoluteFilePath().size()), 16);
    auto reserve = pathBytes ? checked::add(65536 + format::pageSize
        + format::pointCapacity * sizeof(PointRecord), *pathBytes) : std::nullopt;
    if (!reserve || *reserve > limits.memoryBytes) return resource(phase_);
    if (auto s = checkpoint(); !s) return s;
    if (!file_.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
        return makeError(ErrorCode::OpenFailed, phase_, Operation::Open, file_.errorString(), {}, file_.error());
    page_ = std::make_unique<std::byte[]>(format::pageSize);
    if (auto s = envelope(true); !s) return s;
    if (auto s = shape(); !s) return s;
    reserve = checked::add(*reserve, header_.index.height * sizeof(Frame));
    if (!reserve || *reserve > limits.memoryBytes) return resource(phase_);
    reserve_ = *reserve;
    records_ = std::make_unique<PointRecord[]>(format::pointCapacity);
    if (header_.index.height) {
        frames_ = std::make_unique<Frame[]>(header_.index.height);
        const IndexEntry root{header_.metadata.bounds, header_.index.rootPage,
                              header_.metadata.pointCount, PageKind::Index};
        if (auto s = loadIndex(root.childPage, header_.index.height, root); !s) return s;
    }
    return checkpoint();
}

Status CacheReader::checkpoint()
{
    if (cancel_ && cancel_(cancelContext_))
        return makeError(ErrorCode::Cancelled, phase_, Operation::Read, QStringLiteral("Cache reading cancelled"));
    return success();
}
void CacheReader::begin(Phase phase, CancelCheck cancel, void *context)
{
    phase_ = phase;
    cancel_ = cancel;
    cancelContext_ = context;
    statistics_ = {};
    statistics_.bufferBytes = format::pageSize + format::pointCapacity * sizeof(PointRecord)
        + header_.index.height * sizeof(Frame);
    statistics_.traversalFrames = header_.index.height;
    file_.unsetError();
}
Status CacheReader::readAt(std::uint64_t offset, MutableBytes bytes)
{
    if (auto s = checkpoint(); !s) return s;
    if (ioHook_) { if (auto s = ioHook_(*this, Operation::Seek); !s) return s; }
    ++statistics_.seekOperations;
    if (auto s = file::seek(file_, offset); !s) return s;
    if (ioHook_) { if (auto s = ioHook_(*this, Operation::Read); !s) return s; }
    ++statistics_.readOperations;
    if (auto s = file::readExact(file_, bytes); !s) return s;
    statistics_.readBytes += bytes.size();
    return success();
}
Status CacheReader::envelope(bool initial)
{
    if (auto s = checkpoint(); !s) return s;
    auto size = file::size(file_);
    if (!size) return size.error();
    if (!checked::fileOffset(size.value())) return corrupt(phase_);
    MutableBytes bytes{page_.get(), format::headerSize};
    if (auto s = readAt(0, bytes); !s) return s;
    auto header = decodeHeader(bytes);
    if (!header) return header.error();
    auto layout = calculateLayout(header.value().metadata.pointCount, header.value().index.pageCount);
    if (!layout) return layout.error();
    if (size.value() < layout.value().fileSize)
        return makeError(ErrorCode::IncompleteCache, phase_, Operation::Size, QStringLiteral("Truncated cache"));
    if (auto s = readAt(layout.value().footerOffset, bytes); !s) return s;
    auto footer = decodeFooter(bytes);
    if (!footer) return footer.error();
    if (auto s = validateEnvelope(header.value(), footer.value(), size.value()); !s) return s;
    const auto &a = header.value();
    const auto &b = header_;
    if (!initial && (a.metadata.id != b.metadata.id || a.metadata.pointCount != b.metadata.pointCount
        || !equal(a.metadata.bounds, b.metadata.bounds) || a.metadata.hasNonZeroZ != b.metadata.hasNonZeroZ
        || a.index.kind != b.index.kind || a.index.pageCount != b.index.pageCount
        || a.index.rootPage != b.index.rootPage || a.index.height != b.index.height)) return corrupt(phase_);
    if (initial) { header_ = a; layout_ = layout.value(); }
    return success();
}

Status CacheReader::shape()
{
    std::uint64_t children = layout_.pointPages, start = 0, childStart = 0;
    std::uint32_t height = 0;
    while (children) {
        if (height == levels_.size()) return corrupt(phase_);
        const auto pages = *checked::pageCount(children, format::indexCapacity);
        const auto next = checked::add(start, pages);
        if (!next || *next > header_.index.pageCount) return corrupt(phase_);
        levels_[height++] = {start, pages, childStart, children};
        if (pages == 1) {
            if (height != header_.index.height || *next != header_.index.pageCount
                || start != header_.index.rootPage) return corrupt(phase_);
            break;
        }
        childStart = start;
        start = *next;
        children = pages;
    }
    return success();
}

Result<PageHeader> CacheReader::readPage(PageKind kind, std::uint64_t ordinal, std::uint32_t level)
{
    const auto count = kind == PageKind::Points ? layout_.pointPages : header_.index.pageCount;
    const auto start = kind == PageKind::Points ? format::headerSize : layout_.indexOffset;
    const auto length = checked::multiply(ordinal, format::pageSize);
    const auto offset = length ? checked::add(start, *length) : std::nullopt;
    const auto end = kind == PageKind::Points ? layout_.indexOffset : layout_.footerOffset;
    if (ordinal >= count || !offset || !checked::fileOffset(*offset)
        || !checked::range(*offset, format::pageSize, end)) return corrupt(phase_);
    if (auto s = readAt(*offset, {page_.get(), format::pageSize}); !s) return s.error();
    if (kind == PageKind::Points) ++statistics_.pointPages; else ++statistics_.indexPages;
    auto h = validatePage({page_.get(), format::pageSize});
    if (!h) return h.error();
    if (h.value().kind != kind || h.value().pageId != ordinal || h.value().level != level) return corrupt(phase_);
    return h;
}

Status CacheReader::loadIndex(std::uint64_t ordinal, std::uint32_t level, const IndexEntry &expected)
{
    if (!level || level > header_.index.height) return corrupt(phase_);
    const auto &shape = levels_[level - 1];
    if (ordinal < shape.start || ordinal - shape.start >= shape.pages) return corrupt(phase_);
    auto h = readPage(PageKind::Index, ordinal, level);
    if (!h) return h.error();
    const auto first = checked::multiply(ordinal - shape.start, format::indexCapacity);
    if (!first || *first >= shape.children) return corrupt(phase_);
    const auto count = std::min<std::uint64_t>(format::indexCapacity, shape.children - *first);
    if (h.value().count != count) return corrupt(phase_);
    auto &frame = frames_[level - 1];
    frame.count = h.value().count;
    frame.next = 0;
    BoundingBox bounds{};
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < frame.count; ++i) {
        const auto e = decodeIndexEntry(Bytes{page_.get(), format::pageSize}.subspan(
            format::pageHeaderSize + i * format::indexEntrySize, format::indexEntrySize)).value();
        const auto relative = checked::add(*first, i);
        const auto child = relative ? checked::add(shape.childStart, *relative) : std::nullopt;
        const auto sum = checked::add(total, e.pointCount);
        if (!child || e.childPage != *child || !sum || *sum > header_.metadata.pointCount
            || !contains(header_.metadata.bounds, e.bounds)) return corrupt(phase_);
        if (level == 1) {
            const auto before = checked::multiply(e.childPage, format::pointCapacity);
            if (!before || *before >= header_.metadata.pointCount
                || e.pointCount != std::min<std::uint64_t>(format::pointCapacity,
                    header_.metadata.pointCount - *before)) return corrupt(phase_);
        }
        frame.entries[i] = e;
        total = *sum;
        bounds = i ? unite(bounds, e.bounds) : e.bounds;
    }
    if (total != expected.pointCount || !equal(bounds, expected.bounds)) return corrupt(phase_);
    return success();
}

Status CacheReader::points(std::uint64_t ordinal, const IndexEntry *expected,
                           std::span<const BoundingBox> boxes, bool filter, const Sink &sink)
{
    auto h = readPage(PageKind::Points, ordinal, 0);
    if (!h) return h.error();
    const auto before = checked::multiply(ordinal, format::pointCapacity);
    if (!before || *before >= header_.metadata.pointCount || h.value().count !=
        std::min<std::uint64_t>(format::pointCapacity, header_.metadata.pointCount - *before)) return corrupt(phase_);
    BoundingBox bounds{};
    std::size_t used = 0;
    for (std::size_t i = 0; i < h.value().count; ++i) {
        const auto r = decodeRecord(Bytes{page_.get(), format::pageSize}.subspan(
            format::pageHeaderSize + i * format::recordSize, format::recordSize)).value();
        const BoundingBox b{r.point.x, r.point.y, r.point.x, r.point.y};
        if (r.id >= header_.metadata.pointCount || !contains(header_.metadata.bounds, b)) return corrupt(phase_);
        bounds = i ? unite(bounds, b) : b;
        if (!filter || matches(boxes, b)) records_[used++] = r;
    }
    if (expected && (expected->pointCount != h.value().count || !equal(bounds, expected->bounds))) return corrupt(phase_);
    if (auto s = checkpoint(); !s) return s;
    if (used) {
        try {
            auto s = sink({records_.get(), used});
            if (!s) return s;
        } catch (...) {
            return makeError(ErrorCode::ConsumerFailed, phase_, Operation::Consume, QStringLiteral("Cache consumer threw"));
        }
        statistics_.returnedRecords += used;
    }
    return checkpoint();
}

Status CacheReader::walk(std::span<const BoundingBox> boxes, bool filter, const Sink &sink)
{
    if (!header_.metadata.pointCount) return checkpoint();
    const IndexEntry root{header_.metadata.bounds, header_.index.rootPage,
                          header_.metadata.pointCount, PageKind::Index};
    if (auto s = loadIndex(root.childPage, header_.index.height, root); !s) return s;
    auto level = header_.index.height;
    while (true) {
        if (auto s = checkpoint(); !s) return s;
        auto &frame = frames_[level - 1];
        if (frame.next == frame.count) {
            if (level == header_.index.height) break;
            ++level;
            continue;
        }
        const auto e = frame.entries[frame.next++];
        if (filter && !matches(boxes, e.bounds)) continue;
        if (level == 1) {
            if (auto s = points(e.childPage, &e, boxes, filter, sink); !s) return s;
        } else {
            if (auto s = loadIndex(e.childPage, level - 1, e); !s) return s;
            --level;
        }
    }
    return checkpoint();
}

Result<ReadStatistics> CacheReader::scan(std::span<const BoundingBox> boxes, bool spatial,
                                        const Sink &sink, CancelCheck cancel, void *context)
{
    if (active_.exchange(true, std::memory_order_acquire)) return busy();
    ActiveGuard guard{active_};
    begin(Phase::CacheRead, cancel, context);
    try {
        if (!sink) return makeError(ErrorCode::InvalidInput, phase_, Operation::Consume, QStringLiteral("Missing sink"));
        for (auto box : boxes) {
            if (!validBounds(box))
                return makeError(ErrorCode::InvalidInput, phase_, Operation::Read, QStringLiteral("Invalid query bounds"));
            if (auto s = checkpoint(); !s) return s.error();
        }
        if (auto s = checkpoint(); !s) return s.error();
        if (spatial) {
            if (!boxes.empty()) { if (auto s = walk(boxes, true, sink); !s) return s.error(); }
        } else {
            for (std::uint64_t p = 0; p < layout_.pointPages; ++p)
                if (auto s = points(p, nullptr, {}, false, sink); !s) return s.error();
        }
        if (auto s = checkpoint(); !s) return s.error();
        return statistics_;
    } catch (const std::bad_alloc &) { return resource(phase_); }
}
Result<ReadStatistics> CacheReader::scanAll(const Sink &sink, CancelCheck cancel, void *context)
{
    return scan({}, false, sink, cancel, context);
}
Result<ReadStatistics> CacheReader::scanBox(BoundingBox box, const Sink &sink, CancelCheck cancel, void *context)
{
    return scan({&box, 1}, true, sink, cancel, context);
}
Result<ReadStatistics> CacheReader::scanBoxes(std::span<const BoundingBox> boxes, const Sink &sink,
                                            CancelCheck cancel, void *context)
{
    return scan(boxes, true, sink, cancel, context);
}

Result<ReadStatistics> CacheReader::validate(ResourceLimits limits, const QString &parent,
                                           CancelCheck cancel, void *context)
{
    if (active_.exchange(true, std::memory_order_acquire)) return busy();
    ActiveGuard guard{active_};
    begin(Phase::Validation, cancel, context);
    try {
        if (limits.memoryBytes < reserve_ || !limits.maxOpenFiles) return resource(phase_);
        if (auto s = envelope(false); !s) return s.error();
        if (auto s = checkpoint(); !s) return s.error();
        if (!header_.metadata.pointCount) return statistics_;
        const auto paths = checked::multiply(static_cast<std::uint64_t>(QDir(parent).absolutePath().size()), 16);
        const auto arrays = checked::add(reserve_, format::pointCapacity * sizeof(Measurement));
        const auto fixed = paths && arrays ? checked::add(*arrays, *paths) : std::nullopt;
        if (!fixed || limits.memoryBytes <= *fixed || limits.maxOpenFiles < 3) return resource(phase_);
        auto sorted = PointStreamSort::create({limits.memoryBytes - *fixed,
            limits.temporaryDiskBytes, limits.maxOpenFiles - 1}, parent, cancel, context);
        if (!sorted) return sorted.error();
        auto measurements = std::make_unique<Measurement[]>(format::pointCapacity);
        Morton128 previousKey{};
        RecordId previousId = 0;
        bool hasPrevious = false;
        auto traversal = walk({}, false, [&](auto records) -> Status {
            for (std::size_t i = 0; i < records.size(); ++i) {
                const auto key = mortonKey(records[i].point);
                if (hasPrevious && (key < previousKey
                    || (key == previousKey && records[i].id <= previousId))) return corrupt(phase_);
                previousKey = key;
                previousId = records[i].id;
                hasPrevious = true;
                // An injective representation of uint64 ID; each half is exactly
                // representable in binary64. Equal IDs have adjacent Morton keys.
                measurements[i] = {static_cast<double>(records[i].id >> 32),
                    static_cast<double>(records[i].id & UINT32_MAX), 0};
            }
            return sorted.value()->append({measurements.get(), records.size()});
        });
        if (!traversal) return traversal.error();
        std::optional<ModelPoint> previous;
        std::uint64_t count = 0;
        auto ids = sorted.value()->finish([&](auto records) -> Status {
            for (const auto &r : records) {
                if (previous && previous->x == r.point.x && previous->y == r.point.y) return corrupt(phase_);
                previous = r.point;
                ++count;
            }
            return success();
        });
        if (!ids) return ids.error();
        if (count != header_.metadata.pointCount || statistics_.pointPages != layout_.pointPages
            || statistics_.indexPages != header_.index.pageCount) return corrupt(phase_);
        if (auto s = checkpoint(); !s) return s.error();
        return statistics_;
    } catch (const std::bad_alloc &) { return resource(phase_); }
}

} // namespace cadcontour::cache
