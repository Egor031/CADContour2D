#include "project_controller.h"
#include "operation_diagnostics.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include <cassert>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>

namespace cadcontour {
namespace {

WorkerOutcome recoveryFailure(const persistence::Error &error);

} // namespace

class ProjectController::CommandScope {
public:
    explicit CommandScope(ProjectController &controller)
        : controller_(controller), wasExecuting_(std::exchange(controller.executing_, true)) {}
    ~CommandScope() { if (!wasExecuting_) controller_.finishCommand(); }
private:
    ProjectController &controller_;
    bool wasExecuting_;
};

ProjectController::ProjectController(QObject *parent)
    : ProjectController(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                        + QStringLiteral("/projects"), parent) {}

ProjectController::ProjectController(QString storageRoot, QObject *parent)
    : QObject(parent), storageRoot_(QDir::cleanPath(QFileInfo(storageRoot).absoluteFilePath())),
      recoveryRoot_(storageRoot_ + QStringLiteral("/recovery")), runner_(this)
{
    autosaveTimer_.setInterval(60000);
    connect(&autosaveTimer_, &QTimer::timeout, this, [this] { autosave(); });
    autosaveSoon_.setSingleShot(true);
    autosaveSoon_.setInterval(0);
    connect(&autosaveSoon_, &QTimer::timeout, this, [this] { autosave(); });
    autosaveTimer_.start();
    connect(&runner_, &OperationRunner::entered, this, [this](OperationId id) {
        if (id != operation_.id || !operation_.active)
            return;
        if (operation_.status == OperationStatus::Starting)
            operation_.status = OperationStatus::Running;
        operationDiagnostic("worker-entered", operation_, operationClock_.elapsed());
        emit operationChanged();
    });
    connect(&runner_, &OperationRunner::progressAvailable, this, [this](OperationId id, OperationProgress progress) {
        if (id != operation_.id || !operation_.active
            || (operation_.decision != PublicationDecision::Pending
                && !(operation_.kind == OperationKind::DensityBuild && progress.phase == OperationPhase::Cleanup)))
            return;
        const bool phaseChanged = operation_.progress.phase != progress.phase;
        operation_.progress = progress;
        if (phaseChanged)
            operationDiagnostic("phase", operation_, operationClock_.elapsed());
        emit operationChanged();
    });
    connect(&runner_, &OperationRunner::ready, this, &ProjectController::processReady);
    connect(&runner_, &OperationRunner::nonZeroZConfirmationChanged, this, [this](OperationId id) {
        if (id == operation_.id && operation_.active)
            notifyNonZeroZConfirmation();
    });
    connect(&runner_, &OperationRunner::observationFailed, this, [this](OperationId id, quint32 code) {
        if (id != operation_.id || !operation_.active)
            return;
        operation_.completionObservationError = code;
        operationDiagnostic("native-completion-observation-failed", operation_, operationClock_.elapsed());
        emit operationChanged();
    });
    connect(&runner_, &OperationRunner::released, this, &ProjectController::finishOperation);
    connect(&runner_, &OperationRunner::saveFinished, this, &ProjectController::processSaveReceipt);
    connect(&runner_, &OperationRunner::recoveryFinished, this, &ProjectController::processRecoveryReceipt);
}

ProjectController::~ProjectController()
{
    disconnect(zConsumerDestroyed_);
    // Drain before history/state/adapters disappear; no event-loop delivery is required.
    shutdown_ = ShutdownState::Closing;
    operation_.publicationRevoked = true;
    runner_.drain();
    // Low-level destruction preserves recovery. Marker I/O still uses the same
    // managed runner, with no GUI delivery and no simultaneous native worker.
    retireRecovery(shutdownDiscard_);
    if (!retiredRecoveries_.empty()) {
        auto work = [root = recoveryRoot_, retired = std::move(retiredRecoveries_)]
            (const OperationSnapshot &, const WorkerControl &) {
            for (const auto &r : retired) {
                auto result = persistence::closeRecovery(root, r.id, {}, r.lease, r.discard);
                if (!result) return recoveryFailure(result.error());
            }
            return WorkerOutcome::success(nullptr);
        };
        OperationSnapshot cleanup; cleanup.kind = OperationKind::CloseRecovery;
        runner_.start(lastOperationId_, cleanup, std::move(work));
        runner_.drain();
    }
    session_.reset();
}

const ProjectState *ProjectController::project() const noexcept
{
    return session_ ? &session_->state : nullptr;
}

bool ProjectController::isDirty() const noexcept
{
    return session_ && (!session_->saved || !persistence::matchesSavedContent(session_->state, *session_->saved));
}
const persistence::SavedProject *ProjectController::savedProject() const noexcept
{
    return session_ ? session_->saved.get() : nullptr;
}
const persistence::SavedProject *ProjectController::unconfirmedSave() const noexcept
{
    return session_ ? session_->unconfirmed.get() : nullptr;
}

const persistence::CacheAvailability *ProjectController::pointCacheAvailability() const noexcept
{
    return session_ ? session_->availability.get() : nullptr;
}

bool ProjectController::cacheAvailable() const noexcept
{
    return session_ && session_->availability->access == persistence::CacheAccess::AvailableLimited;
}

namespace {
OperationWork openWork(OpenRequest request, std::shared_ptr<const persistence::SavedProject> previous = {})
{
    return [request = std::move(request), previous = std::move(previous)]
        (const OperationSnapshot &, const WorkerControl &control) -> WorkerOutcome {
        control.report({OperationPhase::Preparing, 0, std::nullopt});
        auto cancel = [](void *context) noexcept { return static_cast<const WorkerControl *>(context)->cancelled(); };
        auto result = previous ? persistence::recheckPointCache(*previous, request.readerLimits, cancel,
                                      const_cast<WorkerControl *>(&control))
            : persistence::loadProject(request.manifestPath, request.limits, request.readerLimits, cancel,
                                      const_cast<WorkerControl *>(&control));
        if (control.cancelled()) return WorkerOutcome::cancelled();
        if (!result) {
            auto failure = OperationFailure::make(FailureReason::OpenFailed, OperationPhase::Preparing);
            failure.openError = result.error();
            failure.category = result.error().code == persistence::ErrorCode::ResourceExhausted
                ? FailureCategory::ResourceUnavailable
                : result.error().code == persistence::ErrorCode::FileIO ? FailureCategory::ExecutionFailure
                                                                       : FailureCategory::InvalidInput;
            return WorkerOutcome::failed(std::move(failure));
        }
        control.report({OperationPhase::Finalizing, 0, std::nullopt});
        return WorkerOutcome::success(std::make_shared<const persistence::LoadedProject>(std::move(result.value())));
    };
}
} // namespace

CommandResult ProjectController::openProject(OpenRequest request)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (preemptAutosave()) return CommandResult::rejected(CommandError::Busy);
    if ((request.expectedContext && request.expectedContext != importContext())
        || (session_ && !request.replacementConfirmed))
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ReplacementNotConfirmed);
    if (!QFileInfo(request.manifestPath).isAbsolute() || request.manifestPath.isEmpty()
        || !request.readerLimits.memoryBytes || !request.readerLimits.maxOpenFiles)
        return CommandResult::rejected(CommandError::InvalidValue);
    const auto confirmed = request.replacementConfirmed;
    try { return startWork(openWork(std::move(request)), OperationKind::Open, confirmed); }
    catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
}

CommandResult ProjectController::recheckPointCache(cache::ResourceLimits limits)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (preemptAutosave()) return CommandResult::rejected(CommandError::Busy);
    if (!session_) return CommandResult::rejected(CommandError::NoProject);
    if ((!session_->saved && !session_->recovery) || session_->unconfirmed) return CommandResult::rejected(CommandError::UnsafeState);
    if (!limits.memoryBytes || !limits.maxOpenFiles) return CommandResult::rejected(CommandError::InvalidValue);
    try {
        OpenRequest request;
        request.readerLimits = limits;
        return startWork(openWork(std::move(request), session_->saved ? session_->saved : session_->recovery->state),
                         OperationKind::RecheckCache);
    } catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
}

CommandResult ProjectController::saveProject(SaveRequest request) { return startSave(std::move(request), false); }
CommandResult ProjectController::saveProjectAs(SaveRequest request) { return startSave(std::move(request), true); }

CommandResult ProjectController::startSave(SaveRequest request, bool saveAs)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (!session_) return CommandResult::rejected(CommandError::NoProject);
    if (!persistence::isV1Representable(session_->state))
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::PersistenceVersionsUnsupported);
    if (preemptAutosave()) return CommandResult::rejected(CommandError::Busy);
    if (!cacheAvailable()) return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::DatasetUnavailable);
    if (session_->unconfirmed) return CommandResult::rejected(CommandError::UnsafeState);
    if (session_->recoveryConflict && !saveAs) return CommandResult::rejected(CommandError::UnsafeState);
    if (!session_->state.dataset()) return CommandResult::rejected(CommandError::InvalidValue);
    if (request.manifestPath.isEmpty()) {
        if (saveAs || !session_->saved) return CommandResult::rejected(CommandError::InvalidValue);
        request.manifestPath = session_->saved->manifestPath;
    }
    try {
        auto snapshot = persistence::captureSnapshot(session_->state, QUuid::createUuid(), QUuid::createUuid());
        if (!snapshot) return CommandResult::rejected(CommandError::InvalidValue);
        persistence::SaveInput input{std::move(snapshot.value()), *session_->state.dataset(),
            std::move(request.manifestPath), session_->saved, saveAs, request.limits};
        auto work = [input = std::move(input), prepare = savePrepare_]
            (const OperationSnapshot &, const WorkerControl &control) mutable -> WorkerOutcome {
            control.report({OperationPhase::Preparing, 0, std::nullopt});
            persistence::SaveCallbacks callbacks{[&control] { return control.cancelled(); },
                [&control](std::uint64_t completed, std::uint64_t total) {
                    control.report({OperationPhase::Computing, completed, total});
                }};
            auto result = prepare ? prepare(std::move(input), std::move(callbacks))
                : persistence::SaveTransaction::prepare(std::move(input), std::move(callbacks));
            if (!result) {
                if (result.error().code == persistence::SaveErrorCode::Cancelled) return WorkerOutcome::cancelled();
                auto failure = OperationFailure::make(FailureReason::SaveFailed, OperationPhase::Preparing,
                                                      result.error().detail.data());
                failure.saveError = result.error();
                return WorkerOutcome::failed(failure);
            }
            control.report({OperationPhase::Finalizing, 0, std::nullopt});
            return WorkerOutcome::success(std::move(result.value()));
        };
        return startWork(std::move(work), OperationKind::Save);
    } catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
}

void ProjectController::processSaveReceipt(OperationId id,
    std::shared_ptr<const persistence::SavedProject> prepared, persistence::SaveCommitResult receipt)
{
    if (id != operation_.id || !operation_.active || operation_.kind != OperationKind::Save
        || operation_.decision != PublicationDecision::Accept || !session_
        || snapshot_.contextEpoch != contextEpoch_ || !snapshot_.projectContext
        || snapshot_.projectContext->projectIdentity != session_->state.runtimeIdentity()) return;
    CommandScope guard(*this);
    if (receipt.publication == persistence::SavePublication::Persisted) {
        // All allocations and path construction were completed before permission.
        session_->state.bindDataset(prepared->content.location);
        session_->saved = std::move(prepared);
        session_->recoveryConflict = false;
        emit stateChanged();
    } else if (receipt.publication == persistence::SavePublication::Uncertain) {
        session_->unconfirmed = std::move(prepared);
    }
}

CommandResult ProjectController::checkEntry() const noexcept
{
    if (QThread::currentThread() != thread())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::WrongThread);
    if (shutdown_ != ShutdownState::Open)
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ShuttingDown);
    if (executing_)
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ReentrantCommand);
    return CommandResult::applied();
}

CommandResult ProjectController::newProject()
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ImportRequired);
}

CommandResult ProjectController::closeProject(bool discardConfirmed)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (operation_.active && (operation_.kind == OperationKind::Save || operation_.kind == OperationKind::Autosave)
        && operation_.decision == PublicationDecision::Accept)
        return CommandResult::rejected(CommandError::Busy);
    if (!session_ && (!operation_.active || operation_.decision != PublicationDecision::Pending))
        return CommandResult::noChange();
    if (contextEpoch_ == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ContextEpochExhausted);
    CommandScope guard(*this);
    retireRecovery(discardConfirmed);
    invalidateOperation();
    verification_.reset();
    previousVerification_.reset();
    session_.reset();
    ++contextEpoch_;
    emit stateChanged();
    QTimer::singleShot(0, this, [this] { finishRecoverySessions(); });
    return CommandResult::applied();
}

CommandResult ProjectController::setCell(double value)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (!session_) return CommandResult::rejected(CommandError::NoProject);
    if (!cacheAvailable()) return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::DatasetUnavailable);
    auto owner = session_;
    const auto result = owner->state.checkCellChange(value);
    if (result.outcome != CommandOutcome::Applied) return result;
    if (!owner->history.canAppend())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::HistoryExhausted);
    try {
        auto history = owner->history.prepare({ProjectCommandKind::SetCell, owner->state.cell(), value, {}, {}});
        CommandScope guard(*this);
        if (operation_.kind != OperationKind::Save && operation_.kind != OperationKind::Autosave
            && operation_.kind != OperationKind::DensityBuild) invalidateOperation();
        owner->state.changeCell(value);
        owner->history.publish(history);
        emit stateChanged();
        return result;
    } catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
}

CommandResult ProjectController::setPreciseResetHandler(
    std::function<PreciseResetDecision(const PreciseResetRequest &)> handler)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    preciseResetHandler_ = std::move(handler);
    return CommandResult::applied();
}

PreciseResetRequest ProjectController::densityTransition(DensityTransitionKind action,
    std::optional<VersionId> target) const noexcept
{
    return {action, session_->state.projectId(), session_->state.runtimeIdentity(), contextEpoch_,
        session_->state.activeDensityVersion(), target, session_->preciseGeneration, session_->history.generation(),
        session_->densityChoiceGeneration, session_->state.activeDensityVersion()
            ? session_->state.densityVersion(*session_->state.activeDensityVersion())->rough().revision() : 0};
}

bool ProjectController::transitionApplicable(const PreciseResetRequest &r, bool checkHistory) const noexcept
{
    return shutdown_ == ShutdownState::Open && session_ && contextEpoch_ == r.sessionEpoch
        && session_->state.projectId() == r.projectId && session_->state.runtimeIdentity() == r.runtimeIdentity
        && session_->state.activeDensityVersion() == r.before
        && session_->preciseGeneration == r.preciseGeneration && session_->densityChoiceGeneration == r.choiceGeneration
        && (!checkHistory || session_->history.generation() == r.historyGeneration)
        && (!r.before || session_->state.densityVersion(*r.before)->rough().revision() == r.roughRevision);
}

CommandResult ProjectController::preparePreciseReset(const PreciseResetRequest &request, bool &reset)
{
    reset = false;
    if (!transitionApplicable(request, request.action != DensityTransitionKind::Build))
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::StaleConfirmation);
    if (request.before != request.target
        && session_->densityChoiceGeneration == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    if (request.before == request.target || !session_->preciseResetRequired) return CommandResult::applied();
    if (session_->preciseGeneration == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    auto handler = preciseResetHandler_; // callback may replace its registration
    PreciseResetDecision answer;
    try { answer = handler ? handler(request) : PreciseResetDecision::NeedsConfirmation; }
    catch (const std::bad_alloc &) { throw; }
    catch (...) {
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::PreciseResetPolicyFailed);
    }
    if (!transitionApplicable(request, request.action != DensityTransitionKind::Build)
        || answer == PreciseResetDecision::Stale)
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::StaleConfirmation);
    if (answer != PreciseResetDecision::Confirm)
        return CommandResult::rejected(CommandError::UnsafeState,
            answer == PreciseResetDecision::Cancel ? ErrorDetail::PreciseResetCancelled
                                                    : ErrorDetail::PreciseResetNeedsConfirmation);
    reset = true;
    return CommandResult::applied();
}

void ProjectController::commitPreciseReset(bool reset) noexcept
{
    if (reset) {
        // 06.4 owns only the transition authority. No Precise pipeline or payload
        // exists yet; future stage owners must prepare their retired data before
        // this gate and must never put it in a selection history entry.
        session_->preciseResetRequired = false;
        ++session_->preciseGeneration;
    }
}

CommandResult ProjectController::selectDensityVersion(VersionId id)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (!session_) return CommandResult::rejected(CommandError::NoProject);
    if (id.isNull()) return CommandResult::rejected(CommandError::InvalidValue, ErrorDetail::InvalidDensityVersion);
    auto owner = session_;
    const auto checked = owner->state.checkDensitySelection(id);
    if (checked.outcome != CommandOutcome::Applied) return checked;
    if (owner->densityChoiceGeneration == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    try {
        if (prepareHistoryHook_) prepareHistoryHook_();
        if (session_ != owner || shutdown_ != ShutdownState::Open)
            return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::StaleConfirmation);
        const auto request = densityTransition(DensityTransitionKind::Select, id);
        if (!owner->history.canAppend())
            return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::HistoryExhausted);
        auto history = owner->history.prepare({ProjectCommandKind::DensitySelection, 0, 0, request.before, id});
        bool reset = false;
        const auto guardResult = preparePreciseReset(request, reset);
        if (guardResult.outcome == CommandOutcome::Rejected) return guardResult;
        const auto finalCheck = owner->state.checkDensitySelection(id);
        if (!transitionApplicable(request))
            return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::StaleConfirmation);
        if (finalCheck.outcome != CommandOutcome::Applied) return finalCheck;
        CommandScope guard(*this);
        if (operation_.kind != OperationKind::Save && operation_.kind != OperationKind::Autosave
            && operation_.kind != OperationKind::DensityBuild) invalidateOperation();
        commitPreciseReset(reset);
        owner->state.selectDensityVersion(id);
        ++owner->densityChoiceGeneration;
        owner->history.publish(history);
        emit stateChanged();
        return CommandResult::applied();
    } catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
}

CommandResult ProjectController::undo() { return moveHistory(true); }
CommandResult ProjectController::redo() { return moveHistory(false); }

CommandResult ProjectController::moveHistory(bool undo)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (!session_) return CommandResult::rejected(CommandError::NoProject);
    auto owner = session_;
    if (!(undo ? owner->history.canUndo() : owner->history.canRedo())) {
        if (!cacheAvailable())
            return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::DatasetUnavailable);
        return CommandResult::noChange();
    }
    if (!owner->history.canAdvance())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::HistoryExhausted);
    const auto command = owner->history.command(owner->history.index() - (undo ? 1 : 0));
    const bool selection = command.kind == ProjectCommandKind::DensitySelection;
    if (!selection && !cacheAvailable())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::DatasetUnavailable);
    const auto target = undo ? command.beforeVersion : command.afterVersion;
    const auto result = selection ? owner->state.checkDensitySelection(target)
                                 : owner->state.checkCellChange(undo ? command.beforeCell : command.afterCell);
    if (result.outcome == CommandOutcome::Rejected) return result;
    bool reset = false;
    if (selection) {
        if (owner->densityChoiceGeneration == std::numeric_limits<std::uint64_t>::max())
            return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
        try {
            const auto request = densityTransition(undo ? DensityTransitionKind::Undo : DensityTransitionKind::Redo, target);
            const auto checked = preparePreciseReset(request, reset);
            if (checked.outcome == CommandOutcome::Rejected) return checked;
            if (!transitionApplicable(request))
                return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::StaleConfirmation);
            const auto finalCheck = owner->state.checkDensitySelection(target);
            if (finalCheck.outcome == CommandOutcome::Rejected) return finalCheck;
        } catch (const std::bad_alloc &) {
            return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
        }
    }
    CommandScope guard(*this);
    // Only these two typed commands are independent of the captured Build inputs.
    if (operation_.kind != OperationKind::Save && operation_.kind != OperationKind::Autosave
        && !(operation_.kind == OperationKind::DensityBuild
             && (selection || command.kind == ProjectCommandKind::SetCell))) invalidateOperation();
    commitPreciseReset(reset);
    if (selection) { owner->state.selectDensityVersion(target); ++owner->densityChoiceGeneration; }
    else owner->state.changeCell(undo ? command.beforeCell : command.afterCell);
    owner->history.move(undo);
    emit stateChanged();
    return CommandResult::applied();
}

CommandResult ProjectController::startOperation(OperationWork work,
                                               std::function<void(VerificationState &)> finalize)
{
    const auto result = startWork(std::move(work), OperationKind::Verification);
    if (result.outcome == CommandOutcome::Applied)
        finalize_ = std::move(finalize);
    return result;
}

CommandResult ProjectController::importAscXyz(ImportRequest request)
{
    return startImport(std::move(request), {});
}

namespace {
DensityVersionDescriptor densityDescriptor(const DensityReadyProduct &ready, VersionId id)
{
    const auto &snapshot = *ready.snapshot;
    DensityBuildMetadata build;
    build.tileSide = ready.metadata.tileSide;
    build.groupSide = snapshot.groupSide;
    build.eventCapacity = snapshot.eventCapacity;
    build.resources.workingBytes = snapshot.resources.workingBytes;
    build.resources.metadataBytes = snapshot.resources.metadataBytes;
    build.resources.readerBytes = snapshot.resources.readerBytes;
    build.resources.scratchBytes = snapshot.resources.scratchBytes;
    build.resources.outputBytes = snapshot.resources.outputBytes;
    build.resources.combinedDiskBytes = snapshot.resources.combinedDiskBytes;
    build.resources.files = snapshot.resources.files;
    build.resources.handles = snapshot.resources.handles;
    build.resources.workUnits = snapshot.resources.workUnits;
    build.resources.mappingBoundaryChecks = snapshot.resources.mapping.maxBoundaryChecks;
    build.resources.densityBufferBytes = snapshot.resources.densityBufferBytes;
    build.resources.densityCodecBytes = snapshot.resources.densityCodecBytes;
    build.resources.densitySortBytes = snapshot.resources.densitySortBytes;
    build.statistics.scanned = ready.statistics.scanned;
    build.statistics.eventsWritten = ready.statistics.eventsWritten;
    build.statistics.eventsRead = ready.statistics.eventsRead;
    build.statistics.accumulated = ready.statistics.accumulated;
    build.statistics.tileTotal = ready.statistics.tileTotal;
    build.statistics.scanAllCalls = ready.statistics.scanAllCalls;
    build.statistics.spatialQueries = ready.statistics.spatialQueries;
    build.statistics.occupiedGroups = ready.statistics.occupiedGroups;
    build.statistics.visitedGroups = ready.statistics.visitedGroups;
    build.statistics.tiles = ready.statistics.tiles;
    build.statistics.eventBytesWritten = ready.statistics.eventBytesWritten;
    build.statistics.eventBytesRead = ready.statistics.eventBytesRead;
    build.statistics.eventWrites = ready.statistics.eventWrites;
    build.statistics.eventReads = ready.statistics.eventReads;
    build.statistics.eventOpens = ready.statistics.eventOpens;
    build.statistics.workingPeak = ready.statistics.workingPeak;
    build.statistics.metadataPeak = ready.statistics.metadataPeak;
    build.statistics.scratchPeak = ready.statistics.scratchPeak;
    build.statistics.combinedDiskPeak = ready.statistics.combinedDiskPeak;
    build.statistics.filesPeak = ready.statistics.filesPeak;
    build.statistics.handlesPeak = ready.statistics.handlesPeak;
    build.statistics.workUnits = ready.statistics.workUnits;
    build.statistics.mappingNanoseconds = ready.statistics.mappingNanoseconds;
    build.statistics.sourceOpenNanoseconds = ready.statistics.sourceOpenNanoseconds;
    build.statistics.scanNanoseconds = ready.statistics.scanNanoseconds;
    build.statistics.groupingNanoseconds = ready.statistics.groupingNanoseconds;
    build.statistics.eventSortNanoseconds = ready.statistics.eventSortNanoseconds;
    build.statistics.eventIONanoseconds = ready.statistics.eventIONanoseconds;
    build.statistics.accumulationNanoseconds = ready.statistics.accumulationNanoseconds;
    build.statistics.tileCopyNanoseconds = ready.statistics.tileCopyNanoseconds;
    build.statistics.densityIngestNanoseconds = ready.statistics.densityIngestNanoseconds;
    build.statistics.finishNanoseconds = ready.statistics.finishNanoseconds;
    build.statistics.sourceRead.indexPages = ready.statistics.sourceRead.indexPages;
    build.statistics.sourceRead.pointPages = ready.statistics.sourceRead.pointPages;
    build.statistics.sourceRead.readBytes = ready.statistics.sourceRead.readBytes;
    build.statistics.sourceRead.readOperations = ready.statistics.sourceRead.readOperations;
    build.statistics.sourceRead.seekOperations = ready.statistics.sourceRead.seekOperations;
    build.statistics.sourceRead.returnedRecords = ready.statistics.sourceRead.returnedRecords;
    build.statistics.sourceRead.bufferBytes = ready.statistics.sourceRead.bufferBytes;
    build.statistics.sourceRead.traversalFrames = ready.statistics.sourceRead.traversalFrames;
    build.statistics.density.physicalTiles = ready.statistics.density.physicalTiles;
    build.statistics.density.submissions = ready.statistics.density.submissions;
    build.statistics.density.total = ready.statistics.density.total;
    build.statistics.density.readBytes = ready.statistics.density.readBytes;
    build.statistics.density.writtenBytes = ready.statistics.density.writtenBytes;
    build.statistics.density.decodedTiles = ready.statistics.density.decodedTiles;
    build.statistics.density.indexReads = ready.statistics.density.indexReads;
    build.statistics.density.readOperations = ready.statistics.density.readOperations;
    build.statistics.density.writeOperations = ready.statistics.density.writeOperations;
    build.statistics.density.openOperations = ready.statistics.density.openOperations;
    build.statistics.density.workUnits = ready.statistics.density.workUnits;
    build.statistics.density.bufferReserve = ready.statistics.density.bufferReserve;
    build.statistics.density.codecPeak = ready.statistics.density.codecPeak;
    build.statistics.density.sortReserve = ready.statistics.density.sortReserve;
    build.statistics.density.scratchPeak = ready.statistics.density.scratchPeak;
    build.statistics.density.combinedDiskPeak = ready.statistics.density.combinedDiskPeak;
    build.statistics.density.outputBytes = ready.statistics.density.outputBytes;
    build.statistics.density.mergePasses = ready.statistics.density.mergePasses;
    build.statistics.density.mappingNanoseconds = ready.statistics.density.mappingNanoseconds;
    build.statistics.density.sortNanoseconds = ready.statistics.density.sortNanoseconds;
    build.statistics.density.copyNanoseconds = ready.statistics.density.copyNanoseconds;
    build.statistics.density.readbackNanoseconds = ready.statistics.density.readbackNanoseconds;
    return {id, {ready.metadata.id, std::make_shared<const DensityDatasetLocation>(
        DensityDatasetLocation{std::filesystem::path(ready.completedPath.toStdWString())})},
        ready.metadata.source, ready.metadata.sourceMeasurements, ready.metadata.mapping, build};
}
} // namespace

class ProjectController::VersionCandidate final : public DensityPublicationCandidate {
public:
    VersionCandidate(ProjectController &controller, std::shared_ptr<ProjectSession> owner,
        std::shared_ptr<const DensityReadyProduct> ready, ProjectState::PreparedDensityAppend append,
        ProjectHistory history, PreciseResetRequest request, bool reset,
        std::shared_ptr<const DensityVersion> version)
        : controller_(&controller), owner_(std::move(owner)), ready_(std::move(ready)),
          append_(std::move(append)), history_(std::move(history)), request_(request), reset_(reset), appendVersion_(std::move(version)) {}
    const std::shared_ptr<const DensityReadyProduct> &ready() const noexcept override { return ready_; }
    bool applicable() const noexcept override
    {
        return controller_ && controller_->session_ == owner_ && controller_->transitionApplicable(request_)
            && owner_->state.densityAppendApplicable(append_);
    }
    void commit() noexcept override
    {
        assert(controller_ && controller_->session_ == owner_);
        controller_->commitPreciseReset(reset_);
        owner_->state.commitDensityAppend(append_);
        ++owner_->densityChoiceGeneration;
        owner_->history.publish(history_);
    }
private:
    friend class VersionReceiver;
    QPointer<ProjectController> controller_;
    std::shared_ptr<ProjectSession> owner_;
    std::shared_ptr<const DensityReadyProduct> ready_;
    ProjectState::PreparedDensityAppend append_;
    ProjectHistory history_;
    PreciseResetRequest request_;
    bool reset_;
    std::shared_ptr<const DensityVersion> appendVersion_;
};

class ProjectController::VersionReceiver final : public DensityPublicationReceiver {
public:
    VersionReceiver(ProjectController &controller, const std::shared_ptr<ProjectSession> &owner)
        : controller_(&controller), owner_(owner) {}
    std::unique_ptr<DensityPublicationCandidate> prepare(std::shared_ptr<const DensityReadyProduct> ready) override
    {
        auto owner = owner_.lock();
        if (!controller_ || !owner || controller_->session_ != owner) return {};
        auto &c = *controller_;
        // Fault/reentry seams are outside both the command guard and decision gate.
        if (c.prepareDensityHook_) c.prepareDensityHook_();
        VersionId id;
        if (c.versionIdFactory_) id = c.versionIdFactory_();
        else {
            const auto uuid = QUuid::createUuid().toRfc4122();
            std::memcpy(id.bytes.data(), uuid.constData(), id.bytes.size());
        }
        if (c.prepareHistoryHook_) c.prepareHistoryHook_();
        if (c.session_ != owner || !c.densityApplicable(c.operation_.id)) return {};
        if (!owner->history.canAppend()) throw std::overflow_error("project history exhausted");
        if (owner->densityChoiceGeneration == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Density choice generation exhausted");
        auto version = std::make_shared<const DensityVersion>(densityDescriptor(*ready, id));
        const auto checked = owner->state.checkDensityAppend(*version);
        if (checked.outcome != CommandOutcome::Applied) throw std::invalid_argument("invalid Density version identity or descriptor");
        const auto request = c.densityTransition(DensityTransitionKind::Build, id);
        auto append = owner->state.prepareDensityAppend(version);
        auto history = owner->history.prepare({ProjectCommandKind::DensitySelection, 0, 0, request.before, id});
        // Allocate the final owner before asking about destruction. Confirm alone
        // never destroys Precise or changes current state.
        auto candidate = std::make_unique<VersionCandidate>(c, owner, std::move(ready),
            std::move(append), std::move(history), request, false, std::move(version));
        bool reset = false;
        if (c.preparePreciseReset(request, reset).outcome == CommandOutcome::Rejected) return {};
        if (!c.transitionApplicable(request, false) || !c.densityApplicable(c.operation_.id)) return {};
        if (owner->history.generation() != request.historyGeneration) {
            // Future-cell commands do not revoke this Build or its confirmation.
            // Rebase prepared owners before any destructive reset.
            candidate->append_ = owner->state.prepareDensityAppend(candidate->appendVersion_);
            candidate->history_ = owner->history.prepare(
                {ProjectCommandKind::DensitySelection, 0, 0, request.before, id});
            candidate->request_.historyGeneration = owner->history.generation();
        }
        candidate->reset_ = reset;
        return candidate;
    }
private:
    QPointer<ProjectController> controller_;
    std::weak_ptr<ProjectSession> owner_;
};

CommandResult ProjectController::setDensityReceiver(std::shared_ptr<DensityPublicationReceiver> receiver)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    densityReceiverOverridden_ = true;
    if (receiver == densityReceiver_) return CommandResult::noChange();
    CommandScope guard(*this);
    if (operation_.kind == OperationKind::DensityBuild) invalidateOperation();
    densityReceiver_.swap(receiver); // captured old receiver remains retained by its snapshot
    return CommandResult::applied();
}

CommandResult ProjectController::buildDensity(DensityBuildRequest request)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (preemptAutosave()) return CommandResult::rejected(CommandError::Busy);
    if (!session_) return CommandResult::rejected(CommandError::NoProject);
    if (!densityReceiverOverridden_) {
        try {
            if (!session_->densityReceiver)
                session_->densityReceiver = std::make_shared<VersionReceiver>(*this, session_);
            densityReceiver_ = session_->densityReceiver;
        } catch (const std::bad_alloc &) {
            return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
        }
    }
    if (!densityReceiver_ || !cacheAvailable() || !session_->state.dataset())
        return CommandResult::rejected(CommandError::UnsafeState);
    try {
        auto snapshot = std::make_shared<DensityBuildSnapshot>();
        snapshot->projectId = session_->state.projectId();
        const auto &source = *session_->state.dataset();
        snapshot->source = ImportedDataset{.cacheId = source.cacheId, .location = source.location,
            .pointCount = source.pointCount, .minimum = source.minimum, .maximum = source.maximum};
        snapshot->cell = session_->state.cell();
        snapshot->tileSide = request.tileSide;
        snapshot->groupSide = request.groupSide;
        snapshot->eventCapacity = request.eventCapacity;
        snapshot->resources = request.resources;
        snapshot->outputScope = storageRoot_ + QStringLiteral("/density-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        return startWork(densityBuildWork(snapshot), OperationKind::DensityBuild, false,
                         snapshot, densityReceiver_);
    } catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
}

std::optional<ImportContext> ProjectController::importContext() const noexcept
{
    if (checkEntry().outcome == CommandOutcome::Rejected || runner_.busy())
        return {};
    return importSelectionContext();
}

std::optional<ImportContext> ProjectController::importSelectionContext() const noexcept
{
    // Notifications run inside CommandScope. Read-only intent validation must not
    // confuse that scope, or an Autosave lease, with a change of project inputs.
    if (QThread::currentThread() != thread() || shutdown_ != ShutdownState::Open
        || (runner_.busy() && operation_.kind != OperationKind::Autosave)) return {};
    return fileCommandContext();
}

std::optional<ImportContext> ProjectController::fileCommandContext() const noexcept
{
    if (QThread::currentThread() != thread() || shutdown_ != ShutdownState::Open) return {};
    return ImportContext{session_ ? std::optional(session_->state.operationStamp()) : std::nullopt,
                         contextEpoch_, lastImportContextOperation_};
}

CommandResult ProjectController::setNonZeroZConsumer(QObject *consumer)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (consumer && consumer->thread() != thread())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::WrongThread);
    if (consumer == zConsumer_)
        return CommandResult::noChange();
    if (consumer && runner_.busy())
        return CommandResult::rejected(CommandError::Busy);
    CommandScope guard(*this);
    disconnect(zConsumerDestroyed_);
    zConsumer_ = consumer;
    if (consumer) {
        zConsumerDestroyed_ = connect(consumer, &QObject::destroyed, this, [this] {
            zConsumer_.clear();
            if (operation_.kind == OperationKind::Import)
                cancelOperation();
        });
    } else if (operation_.kind == OperationKind::Import) {
        cancelOperation();
    }
    return CommandResult::applied();
}

std::optional<NonZeroZRequest> ProjectController::pendingNonZeroZ() const
{
    if (QThread::currentThread() != thread() || !zConsumer_ || !applicable(operation_.id))
        return {};
    return runner_.pendingNonZeroZ();
}

CommandResult ProjectController::answerNonZeroZ(const NonZeroZRequest &request, io::ZDecision decision)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!zConsumer_ || !applicable(request.operationId)
        || (decision != io::ZDecision::Continue && decision != io::ZDecision::Cancel))
        return CommandResult::rejected(CommandError::InvalidValue);
    CommandScope guard(*this);
    if (!runner_.answerNonZeroZ(request, decision))
        return CommandResult::rejected(CommandError::InvalidValue);
    if (decision == io::ZDecision::Cancel) {
        operation_.publicationRevoked = true;
        operation_.cancellationRequested = true;
        operation_.status = OperationStatus::Cancelling;
        emit operationChanged();
    }
    notifyNonZeroZConfirmation();
    return CommandResult::applied();
}

void ProjectController::notifyNonZeroZConfirmation()
{
    if (executing_)
        confirmationNotificationPending_ = true;
    else
        emit nonZeroZConfirmationChanged();
}

CommandResult ProjectController::startImport(ImportRequest request, io::ZHandler handler)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (preemptAutosave())
        return CommandResult::rejected(CommandError::Busy);
    if (request.expectedContext && request.expectedContext != importContext())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ReplacementNotConfirmed);
    if (session_ && !request.replacementConfirmed)
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ReplacementNotConfirmed);
    if (request.sourcePath.isEmpty())
        return CommandResult::rejected(CommandError::InvalidValue);
    const auto confirmed = request.replacementConfirmed;
    QString source;
    OperationWork work;
    try {
        request.sourcePath = QFileInfo(request.sourcePath).absoluteFilePath();
        source = request.sourcePath;
        work = ascXyzImportWork(std::move(request), storageRoot_, std::move(handler));
    } catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
    const auto result = startWork(std::move(work), OperationKind::Import, confirmed);
    if (result.outcome == CommandOutcome::Applied)
        importSourcePath_ = source;
    return result;
}

CommandResult ProjectController::startWork(OperationWork work, OperationKind kind, bool confirmed,
    std::shared_ptr<const DensityBuildSnapshot> density, std::shared_ptr<DensityPublicationReceiver> receiver)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected
        && !(kind == OperationKind::CloseRecovery && shutdown_ == ShutdownState::Closing && !executing_))
        return entry;
    if (preemptAutosave())
        return CommandResult::rejected(CommandError::Busy);
    if (kind != OperationKind::Autosave) autosaveSoon_.stop();
    if (kind == OperationKind::Verification && !session_)
        return CommandResult::rejected(CommandError::NoProject);
    if (kind == OperationKind::Verification && !cacheAvailable())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::DatasetUnavailable);
    if (lastOperationId_ == std::numeric_limits<OperationId>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::OperationIdExhausted);
    if (!work)
        return CommandResult::rejected(CommandError::InvalidValue);
    OperationSnapshot snapshot;
    if (session_) {
        snapshot.stamp = session_->state.operationStamp();
        snapshot.cell = session_->state.cell();
        snapshot.projectContext = snapshot.stamp;
    }
    snapshot.kind = kind;
    snapshot.id = lastOperationId_ + 1;
    snapshot.density = std::move(density);
    snapshot.densityReceiver = std::move(receiver);
    snapshot.contextEpoch = contextEpoch_;
    snapshot.replacementConfirmed = confirmed;
    snapshot.nonZeroZConsumerAvailable = !zConsumer_.isNull();
    CommandScope guard(*this);
    snapshot_ = snapshot;
    operation_ = {};
    operation_.id = ++lastOperationId_;
    if (kind != OperationKind::Autosave) lastImportContextOperation_ = operation_.id;
    operation_.stamp = snapshot.stamp;
    operation_.kind = kind;
    operation_.status = OperationStatus::Starting;
    operation_.active = true;
    finalize_ = {};
    operationClock_.start();
    operationDiagnostic("start", operation_);
    if (const auto failure = runner_.start(operation_.id, snapshot, std::move(work)))
        finishOperation(operation_.id, WorkerStatus::Failed, *failure, false);
    else
        emit operationChanged();
    return CommandResult::applied();
}

void ProjectController::invalidateOperation()
{
    if (!operation_.active || operation_.decision != PublicationDecision::Pending)
        return;
    operation_.publicationRevoked = true;
    runner_.cancel();
    notifyNonZeroZConfirmation();
    operationNotificationPending_ = true;
    operationDiagnostic("invalidate", operation_, operationClock_.elapsed());
    // Notification follows settled project state through finishCommand().
}

bool ProjectController::applicable(OperationId id) const noexcept
{
    if (snapshot_.kind == OperationKind::DensityBuild) return densityApplicable(id);
    const auto current = session_ ? std::optional(session_->state.operationStamp()) : std::nullopt;
    const bool sameContext = snapshot_.kind == OperationKind::Save || snapshot_.kind == OperationKind::Autosave
        ? current && snapshot_.projectContext && current->projectIdentity == snapshot_.projectContext->projectIdentity
        : snapshot_.projectContext == current;
    return shutdown_ == ShutdownState::Open && operation_.active
        && id == operation_.id && operation_.decision == PublicationDecision::Pending
        && !operation_.publicationRevoked && !operation_.cancellationRequested
        && snapshot_.contextEpoch == contextEpoch_ && sameContext
        && ((snapshot_.kind != OperationKind::Import && snapshot_.kind != OperationKind::Open
             && snapshot_.kind != OperationKind::RestoreRecovery) || !snapshot_.projectContext
            || snapshot_.replacementConfirmed);
}

bool ProjectController::densityApplicable(OperationId id) const noexcept
{
    if (shutdown_ != ShutdownState::Open || !operation_.active || id != operation_.id
        || snapshot_.id != id || operation_.kind != OperationKind::DensityBuild
        || snapshot_.kind != OperationKind::DensityBuild
        || operation_.decision != PublicationDecision::Pending || operation_.publicationRevoked
        || operation_.cancellationRequested || snapshot_.contextEpoch != contextEpoch_
        || !session_ || !snapshot_.projectContext || !snapshot_.density
        || !snapshot_.densityReceiver || snapshot_.densityReceiver != densityReceiver_
        || snapshot_.projectContext->projectIdentity != session_->state.runtimeIdentity()
        || !cacheAvailable()) return false;
    const auto *current = session_->state.dataset();
    const auto &captured = snapshot_.density->source;
    const auto same = [](double a, double b) noexcept {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    };
    return current && snapshot_.density->projectId == session_->state.projectId()
        && current->location == captured.location && current->cacheId == captured.cacheId
        && current->pointCount == captured.pointCount
        && same(current->minimum.x, captured.minimum.x) && same(current->minimum.y, captured.minimum.y)
        && same(current->maximum.x, captured.maximum.x) && same(current->maximum.y, captured.maximum.y);
}

std::shared_ptr<ProjectController::ProjectSession>
ProjectController::prepareImport(const ImportReadyProduct &product)
{
    const auto &m = product.metadata;
    const auto directory = QDir::cleanPath(product.storageDirectory);
    const auto relative = QDir::cleanPath(product.relativeCachePath);
    if (!cache::validateMetadata(m) || m.pointCount == 0
        || m.pointCount != product.source.measurements
        || m.hasNonZeroZ != product.source.firstNonZeroZ.has_value()
        || product.source.bytesRead == 0 || product.source.physicalLines == 0
        || product.sourcePath != importSourcePath_ || QUuid(product.storageId).isNull()
        || !QDir::isAbsolutePath(directory) || QFileInfo(directory).absolutePath() != storageRoot_
        || !QFileInfo(directory).fileName().startsWith(QStringLiteral("project-") + product.storageId + u'-')
        || QDir::isAbsolutePath(relative) || !relative.startsWith(QStringLiteral("datasets/"))
        || relative.mid(9).contains(u'/') || !relative.endsWith(QStringLiteral(".pc2dcache"))
        || !QFileInfo(QDir(directory).filePath(relative)).isFile())
        throw std::invalid_argument("invalid import metadata or missing cache");
    if (lastIdentity_ == std::numeric_limits<std::uint64_t>::max()
        || contextEpoch_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("project identity/context exhausted");
    ImportedDataset dataset;
    dataset.cacheId = m.id;
    dataset.location = std::make_shared<const DatasetLocation>(DatasetLocation{
        product.storageId.toStdString(), directory.toStdWString(), relative.toStdWString()});
    dataset.sourcePath = product.sourcePath.toStdWString();
    dataset.sourceBytes = product.source.bytesRead;
    dataset.sourceLines = product.source.physicalLines;
    dataset.pointCount = m.pointCount;
    dataset.minimum = {m.bounds.minX, m.bounds.minY};
    dataset.maximum = {m.bounds.maxX, m.bounds.maxY};
    dataset.hasNonZeroZ = m.hasNonZeroZ;
    ProjectId projectId;
    const auto uuid = QUuid::createUuid().toRfc4122();
    std::memcpy(projectId.bytes.data(), uuid.constData(), projectId.bytes.size());
    auto candidate = std::make_shared<ProjectSession>(
        ProjectState(lastIdentity_ + 1, projectId, std::move(dataset)));
    if (prepareImportHook_)
        prepareImportHook_();
    return candidate;
}

void ProjectController::processReady(OperationId id)
{
    if (id != operation_.id || !operation_.active || operation_.decision != PublicationDecision::Pending)
        return;
    if (executing_) {
        completionPending_ = true;
        return; // do not consume Ready while another command owns the guard
    }
    auto lease = runner_.leaseReady(id);
    if (!lease)
        return;
    if (operation_.kind == OperationKind::DensityBuild) {
        processDensityReady(id, lease);
        return;
    }
    CommandScope guard(*this);
    // Every path supplies a decision; lease keeps Discard cleanup on the worker.
    struct DecisionGuard {
        OperationRunner &runner;
        OperationId id;
        ~DecisionGuard() { runner.decide(id, PublicationDecision::Discard); }
    } decisionGuard{runner_, id};
    const auto &outcome = lease.outcome();
    const bool filePublication = operation_.kind == OperationKind::Save || operation_.kind == OperationKind::Autosave
        || operation_.kind == OperationKind::RejectRecovery;
    if (!filePublication || outcome.status != WorkerStatus::Succeeded)
        operation_.workerStatus = outcome.status;
    if (outcome.status == WorkerStatus::Failed)
        operation_.failure = outcome.failure;
    bool committed = false;
    if (outcome.status == WorkerStatus::Succeeded && (applicable(id) || operation_.kind == OperationKind::CloseRecovery)) {
        try {
            if (operation_.kind == OperationKind::Autosave || operation_.kind == OperationKind::RejectRecovery) {
                if (!outcome.recovery || !outcome.recoveryLease)
                    throw std::invalid_argument("expected prepared recovery transaction");
                committed = lease.accept(&operation_, [](void *data) noexcept {
                    static_cast<OperationState *>(data)->decision = PublicationDecision::Accept;
                });
            } else if (operation_.kind == OperationKind::DiscoverRecovery || operation_.kind == OperationKind::CloseRecovery) {
                if (operation_.kind == OperationKind::DiscoverRecovery && !outcome.discovered)
                    throw std::invalid_argument("expected recovery discovery result");
                struct Commit { ProjectController &c; const ReadyOutcome &outcome; } commit{*this, outcome};
                committed = lease.accept(&commit, [](void *data) noexcept {
                    auto &c = *static_cast<Commit *>(data);
                    if (c.outcome.discovered) c.c.recoveryCandidates_ = c.outcome.discovered;
                    c.c.operation_.decision = PublicationDecision::Accept;
                });
            } else if (operation_.kind == OperationKind::Save) {
                if (!outcome.save || outcome.imported || outcome.product)
                    throw std::invalid_argument("expected prepared save transaction");
                committed = lease.accept(&operation_, [](void *data) noexcept {
                    static_cast<OperationState *>(data)->decision = PublicationDecision::Accept;
                }); // permission only; baseline and binding await the persisted receipt
            } else if (operation_.kind == OperationKind::Open || operation_.kind == OperationKind::RecheckCache
                       || operation_.kind == OperationKind::RestoreRecovery) {
                const auto *loadedPointer = outcome.restored ? &outcome.restored->project : outcome.loaded.get();
                if (!loadedPointer || !loadedPointer->saved || !loadedPointer->availability
                    || outcome.product || outcome.imported || outcome.save)
                    throw std::invalid_argument("expected loaded project metadata");
                const auto &loaded = *loadedPointer;
                std::shared_ptr<ProjectSession> candidate;
                if (operation_.kind != OperationKind::RecheckCache) {
                    if (lastIdentity_ == std::numeric_limits<std::uint64_t>::max()
                        || contextEpoch_ == std::numeric_limits<std::uint64_t>::max())
                        throw std::overflow_error("project identity/context exhausted");
                    candidate = std::make_shared<ProjectSession>(ProjectState(lastIdentity_ + 1,
                        loaded.saved->snapshot.projectId, loaded.saved->content, loaded.saved->snapshot.cell));
                    candidate->saved = outcome.restored ? outcome.restored->baseline : loaded.saved;
                    candidate->availability = loaded.availability;
                    if (outcome.restored) {
                        candidate->recoveryId = outcome.restored->source->sessionId;
                        candidate->recovery = outcome.restored->source;
                        candidate->recoveryLease = outcome.restored->lease;
                        candidate->recoveryConflict = outcome.restored->conflict;
                    }
                }
                retiredRecoveries_.reserve(retiredRecoveries_.size() + 1);
                if (prepareOpenHook_) prepareOpenHook_();
                if (applicable(id)) {
                    struct Commit {
                        ProjectController &controller;
                        std::shared_ptr<ProjectSession> &candidate;
                        const persistence::LoadedProject &loaded;
                    } commit{*this, candidate, loaded};
                    committed = lease.accept(&commit, [](void *data) noexcept {
                        auto &c = *static_cast<Commit *>(data);
                        if (c.candidate) {
                            c.controller.session_.swap(c.candidate);
                            if (c.candidate && c.candidate->recovery)
                                c.controller.retiredRecoveries_.push_back({c.candidate->recoveryId,
                                    std::move(c.candidate->recoveryLease), false});
                            ++c.controller.lastIdentity_;
                            ++c.controller.contextEpoch_;
                        } else {
                            if (c.controller.session_->saved) c.controller.session_->saved = c.loaded.saved;
                            c.controller.session_->availability = c.loaded.availability;
                        }
                        c.controller.operation_.decision = PublicationDecision::Accept;
                    });
                    if (committed && operation_.kind != OperationKind::RecheckCache) {
                        verification_.reset();
                        previousVerification_.reset();
                    }
                }
            } else if (operation_.kind == OperationKind::Import) {
                if (!outcome.imported || outcome.product)
                    throw std::invalid_argument("expected import product");
                auto candidate = prepareImport(*outcome.imported);
                retiredRecoveries_.reserve(retiredRecoveries_.size() + 1);
                if (applicable(id)) {
                    struct Commit {
                        ProjectController &controller;
                        std::shared_ptr<ProjectSession> &candidate;
                    } commit{*this, candidate};
                    committed = lease.accept(&commit, [](void *data) noexcept {
                        auto &c = *static_cast<Commit *>(data);
                        c.controller.session_.swap(c.candidate);
                        if (c.candidate && c.candidate->recovery)
                            c.controller.retiredRecoveries_.push_back({c.candidate->recoveryId,
                                std::move(c.candidate->recoveryLease), false});
                        ++c.controller.lastIdentity_;
                        ++c.controller.contextEpoch_;
                        c.controller.operation_.decision = PublicationDecision::Accept;
                    });
                    if (committed) {
                        verification_.reset();
                        previousVerification_.reset();
                    }
                }
            } else {
                if (!outcome.product || outcome.imported)
                    throw std::invalid_argument("expected verification product");
                const auto &product = *outcome.product;
                if (product.sourceCell != session_->state.cell())
                    throw std::invalid_argument("product does not match snapshot cell");
                auto candidate = std::make_unique<VerificationState>();
                if (verification_ && verification_->generation == std::numeric_limits<std::uint64_t>::max())
                    throw std::overflow_error("verification generation exhausted");
                candidate->generation = verification_ ? verification_->generation + 1 : 1;
                candidate->sourceCell = product.sourceCell;
                candidate->value = product.value;
                if (finalize_)
                    finalize_(*candidate); // may fail; receives only the temporary candidate
                if (candidate->sourceCell != product.sourceCell)
                    throw std::invalid_argument("candidate validation failed");
                // No events, dialogs, allocations or throwing history pushes after this check.
                if (applicable(id)) {
                    struct Commit {
                        ProjectController &controller;
                        ReadyProductLease &lease;
                        std::unique_ptr<VerificationState> &candidate;
                    } commit{*this, lease, candidate};
                    committed = lease.accept(&commit, [](void *data) noexcept {
                        auto &c = *static_cast<Commit *>(data);
                        c.candidate->acceptedProduct = c.lease.takeProduct();
                        c.controller.previousVerification_.swap(c.controller.verification_);
                        c.controller.verification_.swap(c.candidate);
                        c.controller.operation_.decision = PublicationDecision::Accept;
                    });
                }
            }
        } catch (const std::bad_alloc &) {
            operation_.failure = OperationFailure::make(FailureReason::OutOfMemory, OperationPhase::Finalizing);
        } catch (const std::invalid_argument &error) {
            operation_.failure = OperationFailure::make(operation_.kind == OperationKind::Import
                ? FailureReason::InvalidProduct : FailureReason::PublicationFailed,
                OperationPhase::Finalizing, error.what());
        } catch (const std::exception &error) {
            operation_.failure = OperationFailure::make(operation_.kind == OperationKind::Import
                ? FailureReason::CandidatePreparationFailed : FailureReason::PublicationFailed,
                                                        OperationPhase::Finalizing, error.what());
        } catch (...) {
            operation_.failure = OperationFailure::make(operation_.kind == OperationKind::Import
                ? FailureReason::CandidatePreparationFailed : FailureReason::PublicationFailed, OperationPhase::Finalizing);
        }
    }
    if (committed) {
        operationDiagnostic("accept", operation_, operationClock_.elapsed());
        if (operation_.kind == OperationKind::DiscoverRecovery)
            emit recoveryChanged();
        else if (!filePublication && operation_.kind != OperationKind::CloseRecovery)
            emit stateChanged(); // save content/binding only changes on persisted receipt
    }
    if (operation_.decision == PublicationDecision::Pending) {
        operation_.decision = PublicationDecision::Discard;
        runner_.decide(id, PublicationDecision::Discard);
        operationDiagnostic("discard", operation_, operationClock_.elapsed());
    }
    operation_.status = OperationStatus::Finishing;
    emit operationChanged();
}

void ProjectController::processDensityReady(OperationId id, ReadyProductLease &lease)
{
    struct InvalidDensityProduct : std::invalid_argument { using std::invalid_argument::invalid_argument; };
    struct DecisionGuard {
        OperationRunner &runner;
        OperationId id;
        ~DecisionGuard() { runner.decide(id, PublicationDecision::Discard); }
    } decisionGuard{runner_, id};
    const auto &outcome = lease.outcome();
    operation_.workerStatus = outcome.status;
    if (outcome.status == WorkerStatus::Failed) operation_.failure = outcome.failure;
    std::unique_ptr<DensityPublicationCandidate> candidate;
    // This binding and the lease survive reentrant Close, detach and Shutdown in prepare.
    const auto receiver = snapshot_.densityReceiver;
    bool committed = false;
    try {
        if (outcome.status == WorkerStatus::Succeeded && densityApplicable(id)) {
            if (!outcome.density || outcome.product || outcome.imported || outcome.save || outcome.loaded
                || outcome.recovery || outcome.recoveryLease || outcome.restored || outcome.discovered
                || outcome.density->snapshot != snapshot_.density
                || !validDensityReady(*snapshot_.density, *outcome.density))
                throw InvalidDensityProduct("invalid density Ready descriptor");
            // Fallible application preparation may refuse a destructive Precise reset.
            // It owns no project mutation and runs without the decision mutex/command guard.
            candidate = receiver->prepare(outcome.density);
            if (candidate && candidate->ready() != outcome.density)
                throw InvalidDensityProduct("density candidate changed its descriptor");
            CommandScope guard(*this);
            if (candidate && densityApplicable(id) && candidate->applicable()) {
                struct Commit { DensityPublicationCandidate &candidate; OperationState &operation; }
                    commit{*candidate, operation_};
                // No callbacks or event delivery between the final gate and this mutex gate.
                committed = lease.accept(&commit, [](void *data) noexcept {
                    auto &value = *static_cast<Commit *>(data);
                    value.candidate.commit();
                    value.operation.decision = PublicationDecision::Accept;
                });
            }
        }
    } catch (const std::bad_alloc &) {
        operation_.failure = OperationFailure::make(FailureReason::OutOfMemory, OperationPhase::Finalizing);
    } catch (const InvalidDensityProduct &error) {
        operation_.failure = OperationFailure::make(FailureReason::InvalidProduct,
                                                   OperationPhase::Finalizing, error.what());
    } catch (const std::exception &error) {
        operation_.failure = OperationFailure::make(FailureReason::CandidatePreparationFailed,
                                                   OperationPhase::Finalizing, error.what());
    } catch (...) {
        operation_.failure = OperationFailure::make(FailureReason::CandidatePreparationFailed,
                                                   OperationPhase::Finalizing);
    }
    {
        CommandScope guard(*this);
        if (!committed) {
            operation_.decision = PublicationDecision::Discard;
            runner_.decide(id, PublicationDecision::Discard);
        }
        operation_.status = OperationStatus::Finishing;
        operationDiagnostic(committed ? "accept" : "discard", operation_, operationClock_.elapsed());
        if (committed) emit stateChanged();
        emit operationChanged();
    }
    // Notifications and candidate/old-owner destruction are outside the decision mutex.
    if (committed) candidate->notify();
}

void ProjectController::finishOperation(OperationId id, WorkerStatus status, OperationFailure failure, bool warning)
{
    if (id != operation_.id || !operation_.active)
        return;
    assert(!runner_.busy()); // Physical release or preparation failure precedes terminal completion.
    CommandScope guard(*this);
    operation_.workerStatus = status;
    if (status == WorkerStatus::Failed && !operation_.failure)
        operation_.failure = failure;
    operation_.cleanupWarning = warning;
    operation_.densityCleanupWarning = failure.densityCleanupError;
    operation_.active = false;
    if (operation_.decision == PublicationDecision::Pending)
        operation_.decision = PublicationDecision::Discard;
    operation_.status = operation_.decision == PublicationDecision::Accept
        && ((operation_.kind != OperationKind::Save && operation_.kind != OperationKind::Autosave
             && operation_.kind != OperationKind::RejectRecovery) || status == WorkerStatus::Succeeded) ? OperationStatus::Succeeded
        : operation_.failure ? OperationStatus::Failed
        : operation_.cancellationRequested || status == WorkerStatus::Cancelled ? OperationStatus::Cancelled
        : OperationStatus::Discarded;
    finalize_ = {};
    if (operation_.kind == OperationKind::CloseRecovery) recoveryClosing_ = false;
    else if (shutdown_ == ShutdownState::Closing) retireRecovery(shutdownDiscard_);
    const bool shutdownReady = shutdown_ == ShutdownState::Closing && retiredRecoveries_.empty();
    if (shutdownReady)
        shutdown_ = ShutdownState::Ready;
    operationDiagnostic("released", operation_, operationClock_.elapsed());
    emit operationChanged();
    if (shutdownReady)
        emit shutdownChanged();
    if (!retiredRecoveries_.empty())
        QTimer::singleShot(0, this, [this] { finishRecoverySessions(); });
    if (shutdown_ == ShutdownState::Open && operation_.status == OperationStatus::Succeeded
        && (operation_.kind == OperationKind::Import || operation_.kind == OperationKind::Save))
        autosaveSoon_.start();
}

CommandResult ProjectController::cancelOperation()
{
    if (QThread::currentThread() != thread())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::WrongThread);
    if (shutdown_ != ShutdownState::Open)
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ShuttingDown);
    if (!operation_.active || operation_.decision != PublicationDecision::Pending
        || operation_.cancellationRequested)
        return CommandResult::noChange();
    operation_.publicationRevoked = true;
    operation_.cancellationRequested = true;
    operation_.status = OperationStatus::Cancelling;
    runner_.cancel();
    operationDiagnostic("cancel", operation_, operationClock_.elapsed());
    emit operationChanged();
    notifyNonZeroZConfirmation();
    return CommandResult::applied();
}

CommandResult ProjectController::requestShutdown(bool discardConfirmed)
{
    if (QThread::currentThread() != thread())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::WrongThread);
    if (shutdown_ != ShutdownState::Open)
        return CommandResult::noChange();
    shutdown_ = ShutdownState::Closing;
    shutdownDiscard_ = discardConfirmed;
    autosaveTimer_.stop();
    autosaveSoon_.stop();
    operation_.publicationRevoked = true;
    if (operation_.active && operation_.decision == PublicationDecision::Pending) {
        operation_.cancellationRequested = true;
        operation_.status = OperationStatus::Cancelling;
    }
    runner_.shutdown(); // cancellation, Discard under mutex, unlock, notify before any wait
    operationDiagnostic("shutdown", operation_, operationClock_.isValid() ? operationClock_.elapsed() : 0);
    // Save receipts arrive after join but before the operation's terminal transition.
    if (!operation_.active && !runner_.busy()) {
        retireRecovery(discardConfirmed);
        if (retiredRecoveries_.empty()) shutdown_ = ShutdownState::Ready;
        else QTimer::singleShot(0, this, [this] { finishRecoverySessions(); });
    }
    emit operationChanged();
    emit shutdownChanged();
    notifyNonZeroZConfirmation();
    return CommandResult::applied();
}

namespace {
WorkerOutcome recoveryFailure(const persistence::Error &error)
{
    auto failure = OperationFailure::make(FailureReason::RecoveryFailed, OperationPhase::Preparing);
    failure.recoveryError = error;
    failure.category = error.code == persistence::ErrorCode::ResourceExhausted
        ? FailureCategory::ResourceUnavailable : error.code == persistence::ErrorCode::FileIO
        ? FailureCategory::ExecutionFailure : FailureCategory::InvalidInput;
    return WorkerOutcome::failed(std::move(failure));
}
} // namespace

bool ProjectController::preemptAutosave()
{
    if (runner_.busy() && operation_.kind == OperationKind::Autosave) cancelOperation();
    return runner_.busy(); // caller retries after native completion; no hidden user-work queue
}

const persistence::RecoveryRecord *ProjectController::recoveryRecord() const noexcept
{ return session_ ? session_->recovery.get() : nullptr; }
bool ProjectController::recoveryRequiresSaveAs() const noexcept
{ return session_ && session_->recoveryConflict; }
const std::vector<persistence::RecoveryCandidate> &ProjectController::recoveryCandidates() const noexcept
{
    static const std::vector<persistence::RecoveryCandidate> empty;
    return recoveryCandidates_ ? *recoveryCandidates_ : empty;
}

CommandResult ProjectController::autosave()
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (runner_.busy()) return CommandResult::rejected(CommandError::Busy);
    if (!session_ || !session_->state.dataset() || recoveryClosing_) return CommandResult::noChange();
    if (!persistence::isV1Representable(session_->state))
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::PersistenceVersionsUnsupported);
    const auto &s = *session_;
    const auto baseline = s.recoveryConflict ? s.recovery->baseline : s.saved;
    if (s.recovery && persistence::matchesSavedContent(s.state, *s.recovery->state)
        && s.recoveryConflict == s.recovery->requiresSaveAs
        && ((!baseline && !s.recovery->baseline)
            || (baseline && s.recovery->baseline && baseline->manifestPath == s.recovery->baseline->manifestPath
                && baseline->manifestDigest == s.recovery->baseline->manifestDigest)))
        return CommandResult::noChange();
    try {
        const auto &dataset = *s.state.dataset();
        if (!dataset.location) return CommandResult::rejected(CommandError::UnsafeState);
        auto snapshot = persistence::captureSnapshot(s.state,
            QUuid(QString::fromStdString(dataset.location->storageId)), QUuid::createUuid());
        if (!snapshot) return CommandResult::rejected(CommandError::InvalidValue);
        auto metadata = std::make_shared<persistence::SavedProject>();
        metadata->snapshot = std::move(snapshot.value()); metadata->content = dataset;
        persistence::RecoveryRecord record{s.recoveryId, persistence::RecoveryDisposition::Open,
            std::move(metadata), baseline, s.recovery ? s.recovery->digest : QByteArray{}, s.recoveryConflict};
        auto work = [root = recoveryRoot_, record = std::move(record), lease = s.recoveryLease, prepare = recoveryPrepare_]
            (const OperationSnapshot &, const WorkerControl &control) mutable {
            auto cancel = [&control] { return control.cancelled(); };
            auto result = prepare ? prepare(root, std::move(record), lease, {}, cancel)
                : persistence::RecoveryTransaction::prepare(root, std::move(record), lease, {}, cancel);
            if (control.cancelled()) return WorkerOutcome::cancelled();
            if (!result) return recoveryFailure(result.error());
            WorkerOutcome outcome; outcome.status = WorkerStatus::Succeeded;
            outcome.recovery = std::move(result.value()); return outcome;
        };
        return startWork(std::move(work), OperationKind::Autosave);
    } catch (const std::bad_alloc &) {
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    }
}

void ProjectController::processRecoveryReceipt(OperationId id,
    std::shared_ptr<const persistence::RecoveryRecord> record,
    std::shared_ptr<const persistence::RecoveryLease> lease, persistence::RecoveryReceipt receipt)
{
    if (id != operation_.id || !operation_.active || operation_.decision != PublicationDecision::Accept
        || !receipt.published) return;
    CommandScope guard(*this);
    if (operation_.kind == OperationKind::Autosave && session_ && snapshot_.contextEpoch == contextEpoch_
        && snapshot_.projectContext && snapshot_.projectContext->projectIdentity == session_->state.runtimeIdentity()
        && record->sessionId == session_->recoveryId) {
        session_->recovery = std::move(record); session_->recoveryLease = std::move(lease);
    }
    recoveryCandidates_.reset();
    emit recoveryChanged();
}

CommandResult ProjectController::discoverRecovery(persistence::RecoveryLimits limits)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (preemptAutosave()) return CommandResult::rejected(CommandError::Busy);
    return startWork([root = recoveryRoot_, limits](const OperationSnapshot &, const WorkerControl &control) {
        auto cancelled = [](void *context) noexcept { return static_cast<const WorkerControl *>(context)->cancelled(); };
        auto result = persistence::discoverRecovery(root, limits, cancelled, const_cast<WorkerControl *>(&control));
        if (control.cancelled()) return WorkerOutcome::cancelled();
        if (!result) return recoveryFailure(result.error());
        WorkerOutcome outcome; outcome.status = WorkerStatus::Succeeded;
        outcome.discovered = std::make_shared<const std::vector<persistence::RecoveryCandidate>>(std::move(result.value()));
        return outcome;
    }, OperationKind::DiscoverRecovery);
}

CommandResult ProjectController::restoreRecovery(RecoveryRequest request)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (preemptAutosave()) return CommandResult::rejected(CommandError::Busy);
    if ((request.expectedContext && request.expectedContext != importContext())
        || (session_ && !request.replacementConfirmed))
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ReplacementNotConfirmed);
    if (!request.candidate.record || !request.readerLimits.memoryBytes || request.readerLimits.maxOpenFiles < 2)
        return CommandResult::rejected(CommandError::InvalidValue);
    const auto confirmed = request.replacementConfirmed;
    return startWork([root = recoveryRoot_, request = std::move(request)]
        (const OperationSnapshot &, const WorkerControl &control) {
        auto cancel = [](void *context) noexcept { return static_cast<const WorkerControl *>(context)->cancelled(); };
        auto result = persistence::restoreRecovery(root, request.candidate, request.limits, request.readerLimits,
            cancel, const_cast<WorkerControl *>(&control));
        if (control.cancelled()) return WorkerOutcome::cancelled();
        if (!result) return recoveryFailure(result.error());
        WorkerOutcome outcome; outcome.status = WorkerStatus::Succeeded;
        outcome.restored = std::make_shared<const persistence::RestoredRecovery>(std::move(result.value()));
        return outcome;
    }, OperationKind::RestoreRecovery, confirmed);
}

CommandResult ProjectController::rejectRecovery(persistence::RecoveryCandidate candidate, persistence::RecoveryLimits limits)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected) return entry;
    if (preemptAutosave()) return CommandResult::rejected(CommandError::Busy);
    if (!candidate.record) return CommandResult::rejected(CommandError::InvalidValue);
    return startWork([root = recoveryRoot_, record = *candidate.record, limits, prepare = recoveryPrepare_]
        (const OperationSnapshot &, const WorkerControl &control) mutable {
        record.disposition = persistence::RecoveryDisposition::Rejected;
        auto cancelled = [&control] { return control.cancelled(); };
        auto transaction = prepare ? prepare(root, std::move(record), {}, limits, cancelled)
            : persistence::RecoveryTransaction::prepare(root, std::move(record), {}, limits, cancelled);
        if (control.cancelled()) return WorkerOutcome::cancelled();
        if (!transaction) return recoveryFailure(transaction.error());
        WorkerOutcome outcome; outcome.status = WorkerStatus::Succeeded;
        outcome.recovery = std::move(transaction.value()); return outcome;
    }, OperationKind::RejectRecovery);
}

void ProjectController::retireRecovery(bool discardConfirmed)
{
    if (!session_ || (!session_->recovery && !(operation_.kind == OperationKind::Autosave
        && operation_.active && operation_.decision == PublicationDecision::Accept))) return;
    for (auto &r : retiredRecoveries_) if (r.id == session_->recoveryId) {
        r.discard = r.discard || discardConfirmed; return;
    }
    retiredRecoveries_.push_back({session_->recoveryId, std::move(session_->recoveryLease), discardConfirmed});
}

void ProjectController::finishRecoverySessions()
{
    if (runner_.busy() || executing_ || retiredRecoveries_.empty() || recoveryClosing_) return;
    auto retired = std::make_shared<std::vector<RetiredRecovery>>();
    retired->swap(retiredRecoveries_);
    recoveryClosing_ = true;
    auto work = [root = recoveryRoot_, retired](const OperationSnapshot &, const WorkerControl &) {
        for (const auto &r : *retired) {
            auto result = persistence::closeRecovery(root, r.id, {}, r.lease, r.discard);
            if (!result) return recoveryFailure(result.error());
        }
        return WorkerOutcome::success(nullptr);
    };
    const auto result = startWork(std::move(work), OperationKind::CloseRecovery);
    if (result.outcome != CommandOutcome::Applied) {
        recoveryClosing_ = false;
        retiredRecoveries_.swap(*retired);
    }
}

void ProjectController::finishCommand()
{
    if (std::exchange(operationNotificationPending_, false))
        emit operationChanged();
    executing_ = false;
    if (std::exchange(confirmationNotificationPending_, false))
        QTimer::singleShot(0, this, [this] { notifyNonZeroZConfirmation(); });
    if (std::exchange(completionPending_, false))
        QTimer::singleShot(0, this, [this] { processReady(operation_.id); });
    if (!std::exchange(transitionNotificationPosted_, true)) {
        QTimer::singleShot(0, this, [this] {
            transitionNotificationPosted_ = false;
            if (!executing_) emit commandTransitionFinished();
            // If nested delivery met another guard, its finishCommand posts the next hint.
        });
    }
}

} // namespace cadcontour
