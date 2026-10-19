#pragma once

#include "project_manifest.h"

namespace cadcontour::persistence {

QString pointCacheRelativePath(QUuid storageId, const cache::CacheId &cacheId);
Status validateDatasetPath(const QString &relativePath);

struct ResolvedDatasetPath {
    QString absolutePath;
    bool exists = false; // availability only; never a content integrity assertion
};
// Existing project directory required. Rejects all descendant reparse points, even
// internal links. Checks existing parents of absent files. No directories are created.
// This is a path observation, not a handle lease: recheck immediately before future I/O.
Result<ResolvedDatasetPath> resolveDatasetPath(const QString &projectDirectory, const QString &relativePath);

} // namespace cadcontour::persistence
