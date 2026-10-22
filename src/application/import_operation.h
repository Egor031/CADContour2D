#pragma once

#include "operation_runner.h"

namespace cadcontour {

struct ImportContext {
    std::optional<OperationStamp> project;
    std::uint64_t epoch = 0;
    OperationId lastOperation = 0; // last explicit/lifecycle operation; Autosave does not revoke intent
    bool operator==(const ImportContext &) const = default;
};

struct ImportRequest {
    QString sourcePath;
    cache::ResourceLimits limits;
    bool replacementConfirmed = false; // authorization for this request and its captured context only
    std::optional<ImportContext> expectedContext; // UI selection context; optional for headless callers
    // When true, worker replaces limits with a bounded profile on the storage volume.
    bool interactiveResources = false;
};

cache::Result<cache::ResourceLimits> interactiveImportLimits(std::uint64_t availableBytes);

OperationWork ascXyzImportWork(ImportRequest request, QString storageRoot,
                              io::ZHandler handler = {});

} // namespace cadcontour
