#pragma once

#include "cache/cache_file.h"
#include "core/raster_mapping.h"

#include <atomic>
#include <vector>

namespace cadcontour::density {

using U64 = std::uint64_t;
struct DatasetId {
    std::array<std::byte, 16> bytes{};
    static DatasetId create();
    bool isNull() const noexcept;
    bool operator==(const DatasetId &) const = default;
};
struct TileKey {
    U64 x = 0, y = 0;
    bool operator==(const TileKey &) const = default;
};
enum class ErrorCode {
    InvalidInput, InvalidState, UnsupportedFormat, CorruptData, IncompleteDataset,
    Mapping, ResourceRefused, Cancelled, IO, ConsumerFailed, CleanupFailed
};
enum class Phase { Metadata, Open, Index, Ingest, Codec, Sort, Merge, Copy, Seal, Flush, Readback, Publish, Read, Validate, Cleanup };
struct Error {
    ErrorCode code;
    Phase phase;
    QString detail; // bounded to 256 UTF-16 units
    std::optional<TileKey> tile;
    std::optional<U64> offset;
    U64 required = 0, budget = 0;
    int deviceCode = 0;
    std::optional<raster::Error> mapping;
};
template<class T> class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}
    explicit operator bool() const noexcept { return std::holds_alternative<T>(data_); }
    T &value() { return std::get<T>(data_); }
    const T &value() const { return std::get<T>(data_); }
    const Error &error() const { return std::get<Error>(data_); }
private:
    std::variant<T, Error> data_;
};
using Status = Result<std::monostate>;
inline Status success() { return std::monostate{}; }
using CancelCheck = raster::Mapping::CancelCheck;
using Diagnostic = void (*)(const Error &, void *) noexcept;

// Explicit caller policy; zero never means unlimited. See density-map-format.md.
struct ResourcePolicy {
    U64 bufferBytes = 0, codecBytes = 0, sortBytes = 0;
    U64 scratchBytes = 0, outputBytes = 0, combinedDiskBytes = 0;
    std::uint32_t openFiles = 0;
    U64 indexRecords = 0, workUnits = 0;
    raster::ValidationPolicy mapping{0};
};
ResourcePolicy initialResourcePolicy() noexcept;

enum class BuildContract { SuppliedTiles, CompletePointCache };
struct Metadata {
    DatasetId id;
    cache::CacheId source;
    U64 sourceMeasurements;
    BuildContract contract;
    std::uint32_t tileSide;
    raster::Mapping mapping;
};
struct Shape { std::uint32_t width, height; bool operator==(const Shape &) const = default; };
// Checked addressing, including keys beyond uint32, without constructing a Mapping.
Result<Shape> tileShape(U64 width, U64 height, std::uint32_t side, TileKey key);
struct Tile { TileKey key; Shape shape; std::vector<U64> counts; };
struct Statistics {
    U64 physicalTiles = 0, submissions = 0, total = 0;
    U64 readBytes = 0, writtenBytes = 0, decodedTiles = 0, indexReads = 0;
    U64 workUnits = 0, bufferReserve = 0, codecPeak = 0, sortReserve = 0;
    U64 scratchPeak = 0, combinedDiskPeak = 0, outputBytes = 0, mergePasses = 0;
    U64 mappingNanoseconds = 0, sortNanoseconds = 0, copyNanoseconds = 0, readbackNanoseconds = 0;
};

class Writer;
class DensityStorageTestAccess;
class PendingDataset {
public:
    PendingDataset(PendingDataset &&) noexcept;
    PendingDataset &operator=(PendingDataset &&) noexcept;
    ~PendingDataset();
    PendingDataset(const PendingDataset &) = delete;
    PendingDataset &operator=(const PendingDataset &) = delete;
    QString path() const noexcept;
    const Metadata &metadata() const noexcept { return metadata_; }
    QString releaseToOwner() noexcept;
private:
    friend class Writer;
    PendingDataset(cache::PublishedFile file, Metadata metadata, Diagnostic diagnostic, void *context);
    void discard() noexcept;
    std::optional<cache::PublishedFile> file_;
    Metadata metadata_;
    Diagnostic diagnostic_;
    void *diagnosticContext_;
};

// Single-use, single-threaded owner; reentrant calls are rejected. Inputs borrowed
// only for each synchronous call. Failure/cancel is terminal, including addTile.
class Writer {
public:
    static Result<std::unique_ptr<Writer>> create(const QString &finalPath, Metadata metadata,
        ResourcePolicy policy, const QString &scratchParent, CancelCheck cancel = nullptr,
        void *context = nullptr, Diagnostic diagnostic = nullptr, void *diagnosticContext = nullptr);
    ~Writer();
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;
    Status addTile(TileKey key, Shape shape, std::span<const U64> counts);
    Result<PendingDataset> finish();
    const Statistics &statistics() const noexcept;
private:
    friend class DensityStorageTestAccess;
    struct Impl;
    explicit Writer(std::unique_ptr<Impl> impl);
    void setFaultHook(Status (*hook)(Phase, void *) noexcept, void *context);
    std::unique_ptr<Impl> impl_;
};

// Owns an independent read-only handle, not the dataset. Operations are guarded
// against concurrent/reentrant use. Retain the immutable file for reader lifetime.
class Reader {
public:
    using Sink = Status (*)(const Tile &, void *) noexcept;
    static Result<std::unique_ptr<Reader>> openReader(const QString &path, ResourcePolicy policy,
        CancelCheck cancel = nullptr, void *context = nullptr);
    ~Reader();
    Reader(const Reader &) = delete;
    Reader &operator=(const Reader &) = delete;
    const Metadata &metadata() const noexcept;
    const raster::Mapping &mapping() const noexcept { return metadata().mapping; }
    const Statistics &statistics() const noexcept;
    Result<Tile> readTile(TileKey key, CancelCheck cancel = nullptr, void *context = nullptr);
    // Borrowed tile lives only during callback. Consumer failure preserves emitted prefix.
    Status forEachTile(Sink sink, void *sinkContext, CancelCheck cancel = nullptr, void *context = nullptr);
    Status fullValidate(CancelCheck cancel = nullptr, void *context = nullptr);
private:
    friend class Writer;
    friend class DensityStorageTestAccess;
    struct Impl;
    explicit Reader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cadcontour::density
