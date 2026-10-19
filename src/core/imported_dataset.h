#pragma once

#include "coordinates.h"
#include "cache_id.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace cadcontour {

// Immutable physical binding. Accepted files are retained even after the last reference
// disappears; only operation-owned unpublished files have automatic cleanup.
struct DatasetLocation {
    std::string storageId;
    std::filesystem::path storageDirectory;
    std::filesystem::path relativeCachePath;
    std::filesystem::path cachePath() const { return storageDirectory / relativeCachePath; }
};

// Domain metadata and a shared retention reference, no file I/O or deletion.
struct ImportedDataset {
    cache::CacheId cacheId;
    std::shared_ptr<const DatasetLocation> location;
    std::filesystem::path sourcePath;
    std::uint64_t sourceBytes = 0;
    std::uint64_t sourceLines = 0;
    std::uint64_t pointCount = 0;
    ModelPoint minimum;
    ModelPoint maximum;
    bool hasNonZeroZ = false;

    std::filesystem::path cachePath() const { return location ? location->cachePath() : std::filesystem::path{}; }
};

} // namespace cadcontour
