#pragma once

#include "operation_types.h"

namespace cadcontour {
void operationDiagnostic(const char *event, const OperationState &state,
                         std::int64_t elapsedMs = 0) noexcept;
}
