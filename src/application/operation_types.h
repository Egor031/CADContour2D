#pragma once

#include "core/project_state.h"
#include "io/asc_xyz_importer.h"
#include "persistence/save_transaction.h"

#include <QTemporaryDir>
#include <QUuid>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace cadcontour {

using OperationId = std::uint64_t;

// Presence in the mailbox means a decision is pending. The token is unique even
// across controller lifetimes, where OperationId starts over. One request per operation.
struct NonZeroZRequest {
    OperationId operationId = 0;
    QUuid requestId;
    std::uint64_t line = 0;
    double value = 0;
    bool operator==(const NonZeroZRequest &) const = default;
};
enum class OperationPhase { Preparing, Computing, Parsing, BuildingCache, Finalizing, Cleanup };
enum class WorkerStatus { Succeeded, Cancelled, Failed };
enum class PublicationDecision { Pending, Accept, Discard };
enum class OperationStatus { Idle, Starting, Running, Cancelling, Finishing,
                             Succeeded, Cancelled, Failed, Discarded };
enum class ShutdownState { Open, Closing, Ready };
enum class FailureCategory { InvalidInput, ResourceUnavailable, ExecutionFailure };
enum class FailureReason { PreparationFailed, LaunchFailed, NativeObservationFailed, OutOfMemory, StandardException,
                           UnknownException, InvalidProduct, PublicationFailed, ImportFailed, CandidatePreparationFailed, SaveFailed };
enum class OperationKind { Verification, Import, Save };

struct OperationFailure {
    FailureCategory category = FailureCategory::ExecutionFailure;
    FailureReason reason = FailureReason::UnknownException;
    OperationPhase phase = OperationPhase::Preparing;
    std::array<char, 192> detail{};
    std::optional<std::uint32_t> systemCode;
    std::optional<cache::Error> importError; // preserves parser/cache category, phase and source location
    std::optional<persistence::SaveError> saveError;
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
    OperationStamp stamp{};
    double cell = 0;
    // No stamp is invented for NoProject. Epoch distinguishes changes of application context.
    std::optional<OperationStamp> projectContext;
    std::uint64_t contextEpoch = 0;
    OperationKind kind = OperationKind::Verification;
    bool replacementConfirmed = false;
    bool nonZeroZConsumerAvailable = false;
    // Actual version identity / Rough revision belong here when those inputs exist.
};

// Task 02 verification transport, isolated from ProjectState and production datasets.
struct VerificationProduct {
    double sourceCell = 1.0;
    std::uint64_t value = 0;
    std::shared_ptr<const void> resource; // optional immutable RAII lifetime token
};

struct ImportReadyProduct {
    cache::CacheMetadata metadata;
    io::SourceSummary source;
    QString sourcePath;
    QString storageId;
    QString storageDirectory;
    QString relativeCachePath;
};

// Worker-only: never installed in OperationContext or borrowed by the application.
// Member order destroys PendingCache before its containing directory guard.
struct WorkerImportProduct {
    std::unique_ptr<QTemporaryDir> directory;
    io::ImportProduct data;
    ImportReadyProduct ready;
};

struct WorkerOutcome {
    WorkerStatus status = WorkerStatus::Failed;
    OperationFailure failure;
    std::unique_ptr<VerificationProduct> product;
    std::unique_ptr<WorkerImportProduct> imported;
    std::unique_ptr<persistence::SaveTransaction> save;
    static WorkerOutcome success(std::unique_ptr<VerificationProduct> product) noexcept;
    static WorkerOutcome success(std::unique_ptr<WorkerImportProduct> product) noexcept;
    static WorkerOutcome success(std::unique_ptr<persistence::SaveTransaction> product) noexcept;
    static WorkerOutcome success(std::nullptr_t) noexcept { return success(std::unique_ptr<VerificationProduct>{}); }
    static WorkerOutcome cancelled() noexcept;
    static WorkerOutcome failed(OperationFailure failure) noexcept;
};

// Only immutable values and the legacy verification token cross the Ready boundary.
struct ReadyOutcome {
    WorkerStatus status = WorkerStatus::Failed;
    OperationFailure failure;
    std::unique_ptr<VerificationProduct> product;
    std::optional<ImportReadyProduct> imported;
    std::shared_ptr<const persistence::SavedProject> save;
};

struct OperationState {
    OperationId id = 0;
    OperationStamp stamp{};
    OperationKind kind = OperationKind::Verification;
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
