#pragma once

#include "point_stream_sort.h"

namespace cadcontour::cache {

// Completed dataset awaiting the application's Accept/Discard decision.
class PendingCache {
public:
    PendingCache(PendingCache &&) noexcept = default;
    PendingCache &operator=(PendingCache &&) noexcept = default;
    PendingCache(const PendingCache &) = delete;
    PendingCache &operator=(const PendingCache &) = delete;
    QString path() const noexcept { return file_.path(); }
    const CacheMetadata &metadata() const noexcept { return header_.metadata; }
    const IndexDescriptor &index() const noexcept { return header_.index; }
    const FileLayout &layout() const noexcept { return layout_; }
    // Caller takes responsibility for retaining/removing the accepted file. No heavy I/O.
    QString releaseToOwner() noexcept { return file_.releaseToOwner(); }
private:
    friend class CacheWriter;
    friend class CacheFileTestAccess;
    PendingCache(PublishedFile file, CacheHeader header, FileLayout layout) noexcept
        : file_(std::move(file)), header_(header), layout_(layout) {}
    PublishedFile file_;
    CacheHeader header_;
    FileLayout layout_;
};

class CacheWriterTestAccess;

// Single-threaded, non-reentrant, single-use. No GUI or application state ownership.
class CacheWriter {
public:
    using CancelCheck = PointStreamSort::CancelCheck;
    static Result<std::unique_ptr<CacheWriter>> create(
        const QString &finalPath, ResourceLimits limits, const QString &temporaryParent,
        CancelCheck cancel = nullptr, void *cancelContext = nullptr);
    ~CacheWriter();
    CacheWriter(const CacheWriter &) = delete;
    CacheWriter &operator=(const CacheWriter &) = delete;
    Status append(std::span<const Measurement> batch);
    Result<PendingCache> finish();
private:
    friend class CacheWriterTestAccess;
    enum class State { Accepting, Finishing, Succeeded, Failed, Cancelled };
    CacheWriter(ResourceLimits limits, CancelCheck cancel, void *context);
    Status initialize(const QString &finalPath, const QString &parent);
    Status available() const;
    Status checkpoint();
    Status fail(Error error);
    bool cleanup();
    Status beforeIO(Operation operation);
    Status diskBudget(std::uint64_t cacheBytes, std::uint64_t descriptorBytes);
    Status writeCache(std::uint64_t offset, Bytes bytes);
    Status readAt(QIODevice &device, std::uint64_t offset, MutableBytes bytes);
    Status writeDescriptor(const IndexEntry &entry);
    Result<IndexEntry> readDescriptor(std::uint64_t ordinal);
    Status consume(std::span<const PointRecord> records);
    Status finishPointPage();
    Result<IndexEntry> summarize(Bytes page, std::uint64_t ordinal, std::uint32_t level);
    Status buildIndex();
    Result<PendingCache> seal();
    Status validate(QIODevice &device, std::uint64_t expectedSize);

    ResourceLimits limits_;
    CancelCheck cancel_;
    void *cancelContext_;
    State state_ = State::Accepting;
    Phase phase_ = Phase::Ingest;
    bool active_ = false;
    std::optional<Error> firstError_;
    std::unique_ptr<PointStreamSort> sorter_;
    std::unique_ptr<StagedFile> staged_;
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<QFile> descriptors_;
    std::unique_ptr<std::byte[]> page_;
    std::unique_ptr<PointRecord[]> records_;
    std::unique_ptr<IndexEntry[]> entries_;
    std::size_t used_ = 0;
    CacheHeader header_;
    FileLayout layout_;
    BoundingBox streamBounds_;
    std::uint64_t streamCount_ = 0, pointPages_ = 0, descriptorBytes_ = 0;
    std::uint64_t writerReserve_ = 0, writerDiskLimit_ = 0;
    std::uint64_t cacheBytes_ = 0, peakDiskBytes_ = 0;
    // Local deterministic fault seam, null in production.
    Status (*ioHook_)(CacheWriter &, Operation) = nullptr;
};

} // namespace cadcontour::cache
