#include "operation_types.h"

#include <cstring>
#include <utility>

namespace cadcontour {

OperationFailure OperationFailure::make(FailureReason reason, OperationPhase phase,
                                       const char *detail) noexcept
{
    OperationFailure result;
    result.reason = reason;
    result.phase = phase;
    if (reason == FailureReason::OutOfMemory || reason == FailureReason::PreparationFailed
        || reason == FailureReason::LaunchFailed || reason == FailureReason::NativeObservationFailed)
        result.category = FailureCategory::ResourceUnavailable;
    if (reason == FailureReason::InvalidProduct)
        result.category = FailureCategory::InvalidInput;
    if (detail) {
        // Bounded copy also works in the bad_alloc path without allocating.
        std::size_t i = 0;
        for (; i + 1 < result.detail.size() && detail[i]; ++i)
            result.detail[i] = detail[i];
    }
    return result;
}

WorkerOutcome WorkerOutcome::success(std::unique_ptr<VerificationProduct> product) noexcept
{
    WorkerOutcome result;
    result.status = WorkerStatus::Succeeded;
    result.product = std::move(product);
    return result;
}
WorkerOutcome WorkerOutcome::cancelled() noexcept
{
    WorkerOutcome result;
    result.status = WorkerStatus::Cancelled;
    return result;
}
WorkerOutcome WorkerOutcome::success(std::unique_ptr<WorkerImportProduct> product) noexcept
{
    WorkerOutcome result;
    result.status = WorkerStatus::Succeeded;
    result.imported = std::move(product);
    return result;
}
WorkerOutcome WorkerOutcome::failed(OperationFailure failure) noexcept
{
    WorkerOutcome result;
    result.failure = failure;
    return result;
}

} // namespace cadcontour
