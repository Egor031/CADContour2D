#pragma once

#include "core/project_state.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace cadcontour {

using OperationId = std::uint64_t;
enum class OperationPhase { Preparing, Computing, Finalizing, Cleanup };
enum class WorkerStatus { Succeeded, Cancelled, Failed };
enum class PublicationDecision { Pending, Accept, Discard };
enum class OperationStatus { Idle, Starting, Running, Cancelling, Finishing,
                             Succeeded, Cancelled, Failed, Discarded };
enum class ShutdownState { Open, Closing, Ready };
enum class FailureCategory { InvalidInput, ResourceUnavailable, ExecutionFailure };
enum class FailureReason { PreparationFailed, LaunchFailed, NativeObservationFailed, OutOfMemory, StandardException,
                           UnknownException, InvalidProduct, PublicationFailed };

struct OperationFailure {
    FailureCategory category = FailureCategory::ExecutionFailure;
    FailureReason reason = FailureReason::UnknownException;
    OperationPhase phase = OperationPhase::Preparing;
    std::array<char, 192> detail{};
    std::optional<std::uint32_t> systemCode;
    static OperationFailure make(FailureReason reason, OperationPhase phase,
                                 const char *detail = nullptr) noexcept;
};

// Fixed-size, latest-value mailbox; neither strings nor per-update Qt events.
struct OperationProgress {
    OperationPhase phase = OperationPhase::Preparing;
    std::uint64_t completed = 0;
    std::optional<std::uint64_t> total;
    bool operator==(const OperationProgress &) const = default;
};

struct OperationSnapshot {
    OperationStamp stamp;
    double cell;
    // Actual version identity / Rough revision belong here when those inputs exist.
};

// Task 02 verification transport, isolated from ProjectState and production datasets.
// Real operations will add their own typed products and publication adapters.
struct VerificationProduct {
    double sourceCell = 1.0;
    std::uint64_t value = 0;
    std::shared_ptr<const void> resource; // optional immutable RAII lifetime token
};

struct WorkerOutcome {
    WorkerStatus status = WorkerStatus::Failed;
    OperationFailure failure;
    std::unique_ptr<VerificationProduct> product;
    static WorkerOutcome success(std::unique_ptr<VerificationProduct> product) noexcept;
    static WorkerOutcome cancelled() noexcept;
    static WorkerOutcome failed(OperationFailure failure) noexcept;
};

struct OperationState {
    OperationId id = 0;
    OperationStamp stamp{};
    OperationStatus status = OperationStatus::Idle;
    OperationProgress progress;
    std::optional<WorkerStatus> workerStatus;
    std::optional<OperationFailure> failure;
    PublicationDecision decision = PublicationDecision::Pending;
    bool active = false;
    bool publicationRevoked = false;
    bool cancellationRequested = false;
    bool cleanupWarning = false;
    std::optional<std::uint32_t> completionObservationError;
};

} // namespace cadcontour
