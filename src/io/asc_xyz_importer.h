#pragma once

#include "asc_xyz_parser.h"
#include "cache/cache_writer.h"

namespace cadcontour::io {

struct ImportProduct {
    cache::PendingCache cache;
    SourceSummary source;
};

enum class ImportPhase { Parsing, BuildingCache };
// Borrowed synchronous observer at two phase boundaries, never per measurement.
using ImportObserver = std::function<void(ImportPhase)>;

// Synchronous headless operation; call from a worker for large inputs. All paths are
// QString (Unicode/UNC). Total limits include parser/source reserve and one source
// handle; the remainder is passed unchanged to CacheWriter's budget protocol.
// Product retains PendingCache cleanup ownership on this same thread. No Accept.
cache::Result<ImportProduct> importAscXyz(
    const QString &sourcePath, const QString &finalCachePath, const QString &temporaryParent,
    cache::ResourceLimits limits, const ZHandler &zHandler,
    CancelCheck cancel = nullptr, void *context = nullptr, const ImportObserver &observer = {});

} // namespace cadcontour::io
