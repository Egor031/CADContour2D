#pragma once

#include "operation_runner.h"

namespace cadcontour {

struct ImportRequest {
    QString sourcePath;
    cache::ResourceLimits limits;
    bool replacementConfirmed = false; // authorization for this request and its captured context only
};

OperationWork ascXyzImportWork(ImportRequest request, QString storageRoot,
                              io::ZHandler handler = {});

} // namespace cadcontour
