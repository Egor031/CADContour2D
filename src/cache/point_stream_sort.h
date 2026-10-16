#pragma once

#include "cache_file.h"
#include "point_cache_format.h"

#include <QFile>
#include <QTemporaryDir>

#include <compare>
#include <functional>
#include <memory>

namespace cadcontour::cache {

struct Measurement { double x = 0; double y = 0; double z = 0; };
struct Morton128 {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    auto operator<=>(const Morton128 &) const = default;
};
// Finite X/Y only. X occupies the more significant bit of each interleaved pair.
Morton128 mortonKey(ModelPoint point) noexcept;

class PointStreamSortTestAccess;

// Single-threaded, single-use operation. Caller owns the input batch and sink state.
// A failed append may have processed a prefix, but makes the entire dataset unusable.
class PointStreamSort {
public:
    using CancelCheck = bool (*)(void *) noexcept;
    using Sink = std::function<Status(std::span<const PointRecord>)>;
    static Result<std::unique_ptr<PointStreamSort>> create(
        ResourceLimits limits, const QString &temporaryParent,
        CancelCheck cancel = nullptr, void *cancelContext = nullptr);
    ~PointStreamSort();
    PointStreamSort(const PointStreamSort &) = delete;
    PointStreamSort &operator=(const PointStreamSort &) = delete;

    Status append(std::span<const Measurement> batch);
    // Span is valid only during the callback. Sink must stage output until success;
    // failure/cancellation can follow an emitted prefix. No callbacks after return.
    // Reentry is rejected. The sink's working memory is outside this operation's budget.
    Result<CacheMetadata> finish(const Sink &sink);

private:
    friend class PointStreamSortTestAccess;
    struct Entry {
        Morton128 key;
        PointRecord record;
    };
    struct Run { std::uint64_t offset = 0, count = 0, checksum = 0; };
    struct Generation { std::uint64_t bytes = 0, runs = 0; };
    struct Cursor {
        Run run;
        std::uint64_t read = 0, checksum = 0;
        std::size_t position = 0, available = 0;
        Entry previous{};
        bool hasPrevious = false;
    };
    enum class State { Accepting, Consuming, Succeeded, Failed, Cancelled };
    struct Statistics {
        std::uint64_t peakBufferBytes = 0, liveBufferBytes = 0, peakPlannedDiskBytes = 0;
        std::uint64_t initialRuns = 0, mergeGenerations = 0;
        std::uint32_t openFiles = 0, peakOpenFiles = 0;
    };
    PointStreamSort(ResourceLimits limits, CancelCheck cancel, void *context);
    Status initialize(const QString &parent);
    Status checkState() const;
    Status checkpoint();
    Status fail(Error error);
    bool cleanup();
    void releaseBuffers();
    void accountBuffers(std::uint64_t bytes);
    Status beforeIO(Operation operation, QFile &file);
    Status open(std::unique_ptr<QFile> &file, const QString &path);
    void close(std::unique_ptr<QFile> &file);
    Status readAt(QFile &file, std::uint64_t offset, MutableBytes bytes);
    Status write(QFile &file, Bytes bytes);
    Status flush(QFile &file);
    Status checkSize(QFile &file, std::uint64_t expected);
    Status diskBudget(std::uint64_t source, std::uint64_t destination);
    Status spill();
    Result<Run> readRun(QFile &file, const Generation &generation, std::uint64_t &offset);
    Status finishRun(QFile &file, std::uint64_t offset, std::uint64_t count, std::uint64_t checksum);
    Status allocateMerge();
    Status refill(QFile &file, Cursor &cursor, Entry *buffer);
    Status mergeGroup(QFile &source, std::size_t count, QFile *destination, const Sink *sink);
    Status verifyGeneration(QFile &file, const Generation &generation);
    Status consume(const Sink &sink);

    ResourceLimits limits_;
    CancelCheck cancel_ = nullptr;
    void *cancelContext_ = nullptr;
    State state_ = State::Accepting;
    Phase phase_ = Phase::Ingest;
    bool active_ = false;
    std::optional<Error> firstError_;
    CacheMetadata metadata_;
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<QFile> source_, destination_;
    Generation generation_;
    bool alternate_ = false;
    std::unique_ptr<Entry[]> entries_, inputs_;
    std::unique_ptr<std::byte[]> transfer_;
    std::unique_ptr<Cursor[]> cursors_;
    std::unique_ptr<std::size_t[]> heap_;
    std::unique_ptr<PointRecord[]> output_;
    std::size_t runCapacity_ = 0, used_ = 0, blockRecords_ = 0, fanIn_ = 0;
    std::uint64_t fixedBytes_ = 0;
    Statistics statistics_;
    // Local fault seam, called before actual file operations; production leaves it null.
    Status (*ioHook_)(PointStreamSort &, Operation, QFile &) = nullptr;
};

} // namespace cadcontour::cache
