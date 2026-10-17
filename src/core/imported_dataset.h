#pragma once

#include "coordinates.h"
#include "cache_id.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace cadcontour {

// Domain metadata only. Retention of the referenced immutable file belongs to storage.
struct ImportedDataset {
    cache::CacheId cacheId;
    std::string storageId;
    std::filesystem::path storageDirectory;
    std::filesystem::path relativeCachePath;
    std::filesystem::path sourcePath;
    std::uint64_t sourceBytes = 0;
    std::uint64_t sourceLines = 0;
    std::uint64_t pointCount = 0;
    ModelPoint minimum;
    ModelPoint maximum;
    bool hasNonZeroZ = false;

    std::filesystem::path cachePath() const { return storageDirectory / relativeCachePath; }
};

} // namespace cadcontour
