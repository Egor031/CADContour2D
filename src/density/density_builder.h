#pragma once

#include "density_storage.h"
#include "cache/cache_reader.h"
#include "core/imported_dataset.h"

namespace cadcontour::density {

enum class BuildPhase { Preflight, Mapping, SourceOpen, Scan, Grouping, Accumulation, TileCopy, DensityIngest, Finish, Cleanup, Complete };
enum class BuildErrorCode { InvalidInput, EmptySource, SourceUnavailable, SourceMismatch, CacheFailure,
    Mapping, ResourceRefused, Cancelled, ScratchIO, OutputIO, Overflow, InconsistentEvents, DensityFailure, CleanupFailure };
struct BuildError {
    BuildErrorCode code;
    BuildPhase phase;
    QString detail; // at most 256 UTF-16 units
    std::optional<TileKey> group, tile;
    std::optional<U64> recordId, offset;
    std::optional<ModelPoint> point;
    U64 required = 0, budget = 0;
    std::optional<cache::Error> cache;
    std::optional<raster::Error> mapping;
    std::optional<Error> density;
};
struct BuildProgress { BuildPhase phase; U64 scanned = 0, eventsRead = 0, groups = 0, tiles = 0; };
using BuildProgressCallback = void (*)(const BuildProgress &, void *) noexcept;
using BuildDiagnostic = void (*)(const BuildError &, void *) noexcept;

// All ceilings are explicit: zero permits no use of that resource (never unlimited).
// RAM counts owned allocation requests/reservations, not process/allocator/OS overhead.
// metadataBytes is a sub-ceiling of workingBytes. Disk counts logical file lengths.
// Work: one unit per point, sort comparison, group, tile-copy row and spool I/O chunk,
// plus component cancellation checkpoints. Mapping has its own boundary-check ceiling.
struct BuildResourcePolicy {
    U64 workingBytes = 0, metadataBytes = 0;
    U64 readerBytes = 0; // exclusively reserved until scanAll and reader destruction
    U64 scratchBytes = 0, outputBytes = 0, combinedDiskBytes = 0;
    U64 files = 0; // simultaneously existing owned files, plus the retained source
    std::uint32_t handles = 0;
    U64 workUnits = 0;
    raster::ValidationPolicy mapping{0};
    // Component reservations, deducted from working RAM/disk/handles rather than
    // giving D the top-level ceiling. mapping and work are assigned by the builder.
    U64 densityBufferBytes = 0, densityCodecBytes = 0, densitySortBytes = 0;
};
BuildResourcePolicy initialBuildResourcePolicy() noexcept;

struct BuildRequest {
    ImportedDataset source; // immutable snapshot; retained physical cachePath must be absolute
    double cell = 1;
    std::uint32_t tileSide = 256, groupSide = 512;
    U64 eventCapacity = 16384;
    QString outputPath, scratchParent; // absolute paths; scratchParent must already exist
    BuildResourcePolicy resources;
    // Borrowed only until build returns; callbacks must not throw or reenter.
    CancelCheck cancel = nullptr;
    BuildProgressCallback progress = nullptr;
    void *context = nullptr;
    // Retained through late PendingDataset destruction; nullptr context is allowed.
    BuildDiagnostic diagnostic = nullptr;
    std::shared_ptr<void> diagnosticContext;
};
struct BuildStatistics {
    U64 scanned = 0, eventsWritten = 0, eventsRead = 0, accumulated = 0, tileTotal = 0;
    U64 scanAllCalls = 0, spatialQueries = 0, occupiedGroups = 0, visitedGroups = 0, tiles = 0;
    U64 eventBytesWritten = 0, eventBytesRead = 0, eventWrites = 0, eventReads = 0, eventOpens = 0;
    U64 workingPeak = 0, metadataPeak = 0, scratchPeak = 0, combinedDiskPeak = 0, filesPeak = 0, handlesPeak = 0, workUnits = 0;
    U64 mappingNanoseconds = 0, sourceOpenNanoseconds = 0, scanNanoseconds = 0,
        groupingNanoseconds = 0, eventSortNanoseconds = 0, eventIONanoseconds = 0,
        accumulationNanoseconds = 0, tileCopyNanoseconds = 0, densityIngestNanoseconds = 0, finishNanoseconds = 0;
    cache::ReadStatistics sourceRead;
    Statistics density;
};
struct BuildOutput {
    PendingDataset dataset;
    BuildStatistics statistics;
};
class BuildResult {
public:
    BuildResult(BuildOutput output) : data_(std::move(output)) {}
    BuildResult(BuildError error) : data_(std::move(error)) {}
    explicit operator bool() const noexcept { return std::holds_alternative<BuildOutput>(data_); }
    BuildOutput &value() { return std::get<BuildOutput>(data_); }
    const BuildOutput &value() const { return std::get<BuildOutput>(data_); }
    const BuildError &error() const { return std::get<BuildError>(data_); }
private:
    std::variant<BuildOutput, BuildError> data_;
};

class DensityBuilderTestAccess;
class DensityBuilder {
public:
    static BuildResult build(const BuildRequest &request);
private:
    friend class DensityBuilderTestAccess;
    // Narrow operation-local fault seam. Path belongs to the operation; tests may
    // corrupt it at Reopen/Read. Called before real I/O, never claims physical failure.
    enum class FaultPoint { Create, Append, Reopen, Read, Remove, AfterFinish,
        DensityIngest, DensitySort, DensitySeal, DensityReadback };
    using FaultHook = std::optional<BuildError> (*)(FaultPoint, const QString &, void *) noexcept;
    static BuildResult buildWithHook(const BuildRequest &, FaultHook, void *);
    using TransferHook = std::optional<qint64> (*)(bool writing, QFile &, cache::MutableBytes, void *) noexcept;
    static BuildResult buildWithTransferHook(const BuildRequest &, FaultHook, void *, TransferHook, void *);
};

// Checked group addressing usable without allocating/validating a raster area.
struct BuildGroupAddress { TileKey key; std::uint32_t localIndex; Shape shape; };
Result<BuildGroupAddress> buildGroupAddress(U64 width, U64 height, std::uint32_t groupSide,
    std::uint32_t tileSide, raster::CellIndex cell);
Result<U64> buildCheckedCountAdd(U64 value, U64 increment);

} // namespace cadcontour::density
