#pragma once

#include "save_transaction.h"
#include "cache/cache_reader.h"

namespace cadcontour::persistence {

enum class CacheAccess { AvailableLimited, Missing, Corrupt, Unsupported, FileIO, UnsafePath };
// A time-local observation. AvailableLimited never asserts page/RecordId integrity.
struct CacheAvailability {
    CacheAccess access = CacheAccess::AvailableLimited;
    std::optional<cache::Error> cacheError;
    std::optional<Error> pathError;
    cache::ReadStatistics reads;
};
struct LoadedProject {
    std::shared_ptr<const SavedProject> saved;
    std::shared_ptr<const CacheAvailability> availability;
};

// Read-only, bounded metadata I/O. No source access, scans, copies or temporary files.
Result<LoadedProject> loadProject(const QString &manifestPath, ManifestLimits limits,
    cache::ResourceLimits readerLimits, cache::CacheReader::CancelCheck cancel = nullptr, void *context = nullptr);
// Reuses the exact published content/digest; only the observed file identity is refreshed.
Result<LoadedProject> recheckPointCache(const SavedProject &saved, cache::ResourceLimits readerLimits,
    cache::CacheReader::CancelCheck cancel = nullptr, void *context = nullptr);

// Full integrity validation is a separate explicit caller operation:
// CacheReader::open + validate(limits, temporaryParent, cancel, context), on a worker.
// Neither function above invokes it or grants a fully validated state.

} // namespace cadcontour::persistence
