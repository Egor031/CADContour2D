#pragma once

#include "cache_id.h"
#include "density_dataset_id.h"
#include "raster_mapping.h"
#include "version_id.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

namespace cadcontour {

// Retention only: no open handle or automatic deletion of an accepted dataset.
struct DensityDatasetLocation {
    std::filesystem::path path;
};
struct DensityDatasetReference {
    density::DatasetId id;
    std::shared_ptr<const DensityDatasetLocation> location;
};

// Scalar provenance copied at the application boundary. These domain records do
// not depend on storage readers, workers or their resource-owning result types.
struct DensitySourceReadStatistics {
    std::uint64_t indexPages = 0, pointPages = 0, readBytes = 0;
    std::uint64_t readOperations = 0, seekOperations = 0, returnedRecords = 0;
    std::uint64_t bufferBytes = 0;
    std::uint32_t traversalFrames = 0;
};
struct DensityStorageStatistics {
    std::uint64_t physicalTiles = 0, submissions = 0, total = 0;
    std::uint64_t readBytes = 0, writtenBytes = 0, decodedTiles = 0, indexReads = 0;
    std::uint64_t readOperations = 0, writeOperations = 0, openOperations = 0;
    std::uint64_t workUnits = 0, bufferReserve = 0, codecPeak = 0, sortReserve = 0;
    std::uint64_t scratchPeak = 0, combinedDiskPeak = 0, outputBytes = 0, mergePasses = 0;
    std::uint64_t mappingNanoseconds = 0, sortNanoseconds = 0, copyNanoseconds = 0, readbackNanoseconds = 0;
};
struct DensityBuildStatistics {
    std::uint64_t scanned = 0, eventsWritten = 0, eventsRead = 0, accumulated = 0, tileTotal = 0;
    std::uint64_t scanAllCalls = 0, spatialQueries = 0, occupiedGroups = 0, visitedGroups = 0, tiles = 0;
    std::uint64_t eventBytesWritten = 0, eventBytesRead = 0, eventWrites = 0, eventReads = 0, eventOpens = 0;
    std::uint64_t workingPeak = 0, metadataPeak = 0, scratchPeak = 0, combinedDiskPeak = 0,
        filesPeak = 0, handlesPeak = 0, workUnits = 0;
    std::uint64_t mappingNanoseconds = 0, sourceOpenNanoseconds = 0, scanNanoseconds = 0,
        groupingNanoseconds = 0, eventSortNanoseconds = 0, eventIONanoseconds = 0,
        accumulationNanoseconds = 0, tileCopyNanoseconds = 0, densityIngestNanoseconds = 0, finishNanoseconds = 0;
    DensitySourceReadStatistics sourceRead;
    DensityStorageStatistics density;
};
struct DensityBuildResources {
    std::uint64_t workingBytes = 0, metadataBytes = 0, readerBytes = 0;
    std::uint64_t scratchBytes = 0, outputBytes = 0, combinedDiskBytes = 0, files = 0;
    std::uint32_t handles = 0;
    std::uint64_t workUnits = 0, mappingBoundaryChecks = 0;
    std::uint64_t densityBufferBytes = 0, densityCodecBytes = 0, densitySortBytes = 0;
};
enum class DensityBuildMethod { C1 };
enum class DensityCountContract { CompletePointCache };
struct DensityBuildMetadata {
    DensityBuildMethod method = DensityBuildMethod::C1;
    DensityCountContract contract = DensityCountContract::CompletePointCache;
    std::uint32_t densityFormatVersion = 1;
    std::uint32_t tileSide = 0, groupSide = 0;
    std::uint64_t eventCapacity = 0;
    DensityBuildResources resources;
    DensityBuildStatistics statistics;
};
struct DensityVersionDescriptor {
    VersionId id;
    DensityDatasetReference dataset;
    cache::CacheId source;
    std::uint64_t sourceMeasurements;
    raster::Mapping mapping;
    DensityBuildMetadata build;
};

// The state is deliberately empty until Rough tools exist. Its owner is already
// per-version, stable and separate from the immutable counts descriptor.
class RoughState {
public:
    std::uint64_t revision() const noexcept { return revision_; }
private:
    std::uint64_t revision_ = 0;
};

class DensityVersion {
public:
    explicit DensityVersion(DensityVersionDescriptor descriptor)
        : descriptor_(std::move(descriptor)), rough_(std::make_unique<RoughState>()) {}
    DensityVersion(const DensityVersion &) = delete;
    DensityVersion &operator=(const DensityVersion &) = delete;
    const DensityVersionDescriptor &descriptor() const noexcept { return descriptor_; }
    const RoughState &rough() const noexcept { return *rough_; }
private:
    DensityVersionDescriptor descriptor_;
    std::unique_ptr<RoughState> rough_;
};

} // namespace cadcontour
