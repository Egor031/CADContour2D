#pragma once

#include "point_stream_sort.h"

#include <atomic>

namespace cadcontour::cache {

struct ReadStatistics {
    std::uint64_t indexPages = 0, pointPages = 0, readBytes = 0;
    std::uint64_t readOperations = 0, seekOperations = 0, returnedRecords = 0;
    std::uint64_t bufferBytes = 0;
    std::uint32_t traversalFrames = 0;
};

class CacheReaderTestAccess;

// Owns a read-only handle, never the dataset. The owner must retain the immutable file.
// One operation at a time; concurrent/reentrant calls return InvalidState.
class CacheReader {
public:
    using CancelCheck = PointStreamSort::CancelCheck;
    using Sink = PointStreamSort::Sink;
    static Result<std::unique_ptr<CacheReader>> open(
        const QString &path, ResourceLimits limits,
        CancelCheck cancel = nullptr, void *context = nullptr);
    ~CacheReader() = default;
    CacheReader(const CacheReader &) = delete;
    CacheReader &operator=(const CacheReader &) = delete;

    const CacheMetadata &metadata() const noexcept { return header_.metadata; }
    const IndexDescriptor &index() const noexcept { return header_.index; }
    const FileLayout &layout() const noexcept { return layout_; }
    // Borrowed batches (<= pointCapacity) live only during the callback. Failure or
    // cancellation can follow an emitted prefix. A subsequent operation starts afresh.
    Result<ReadStatistics> scanAll(const Sink &sink, CancelCheck cancel = nullptr, void *context = nullptr);
    Result<ReadStatistics> scanBox(BoundingBox box, const Sink &sink,
                                  CancelCheck cancel = nullptr, void *context = nullptr);
    Result<ReadStatistics> scanBoxes(std::span<const BoundingBox> boxes, const Sink &sink,
                                    CancelCheck cancel = nullptr, void *context = nullptr);
    // Total working budget includes the already allocated reader reserve and one
    // dataset handle. ID validation uses the remaining budget for PointStreamSort.
    Result<ReadStatistics> validate(ResourceLimits limits, const QString &temporaryParent,
                                   CancelCheck cancel = nullptr, void *context = nullptr);
private:
    friend class CacheReaderTestAccess;
    struct Level { std::uint64_t start = 0, pages = 0, childStart = 0, children = 0; };
    struct Frame {
        std::array<IndexEntry, format::indexCapacity> entries;
        std::uint32_t count = 0, next = 0;
    };
    explicit CacheReader(const QString &path) : file_(path) {}
    Status initialize(ResourceLimits limits);
    Status envelope(bool initial);
    Status shape();
    Status checkpoint();
    Status readAt(std::uint64_t offset, MutableBytes bytes);
    Result<PageHeader> readPage(PageKind kind, std::uint64_t ordinal, std::uint32_t level);
    Status loadIndex(std::uint64_t ordinal, std::uint32_t level, const IndexEntry &expected);
    Status points(std::uint64_t ordinal, const IndexEntry *expected,
                  std::span<const BoundingBox> boxes, bool filter, const Sink &sink);
    Status walk(std::span<const BoundingBox> boxes, bool filter, const Sink &sink);
    Result<ReadStatistics> scan(std::span<const BoundingBox> boxes, bool spatial,
                                const Sink &sink, CancelCheck cancel, void *context);
    void begin(Phase phase, CancelCheck cancel, void *context);

    QFile file_;
    CacheHeader header_;
    FileLayout layout_;
    // More than enough even for a binary hierarchy in the qint64 file range.
    std::array<Level, 64> levels_{};
    std::unique_ptr<std::byte[]> page_;
    std::unique_ptr<PointRecord[]> records_;
    std::unique_ptr<Frame[]> frames_;
    std::uint64_t reserve_ = 0;
    std::atomic<bool> active_ = false;
    Phase phase_ = Phase::CacheRead;
    CancelCheck cancel_ = nullptr;
    void *cancelContext_ = nullptr;
    ReadStatistics statistics_;
    Status (*ioHook_)(CacheReader &, Operation) = nullptr;
};

} // namespace cadcontour::cache
