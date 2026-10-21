#include "project_controller.h"
#include "operation_diagnostics.h"

#include <QUndoCommand>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include <cassert>
#include <cstring>
#include <limits>
#include <memory>

namespace cadcontour {
namespace {

WorkerOutcome recoveryFailure(const persistence::Error &error);

class SetCellCommand final : public QUndoCommand {
public:
    SetCellCommand(ProjectState &state, double value)
        : state_(state), before_(state.cell()), after_(value)
    {}

    void undo() override { apply(before_); }
    void redo() override { apply(after_); }

private:
    void apply(double value) noexcept
    {
        // Controller preflights every push/undo/redo before moving the stack.
        const auto result = state_.changeCell(value);
        assert(result.outcome == CommandOutcome::Applied);
        (void)result;
    }

    ProjectState &state_;
    double before_;
    double after_;
};

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
        if (id != operation_.id || !operation_.active || operation_.decision != PublicationDecision::Pending)
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
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!session_)
        return CommandResult::rejected(CommandError::NoProject);
    if (!cacheAvailable())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::DatasetUnavailable);
    const auto result = session_->state.checkCellChange(value);
    if (result.outcome != CommandOutcome::Applied)
        return result;
    if (session_->history.index() == std::numeric_limits<int>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::HistoryExhausted);
    auto command = std::make_unique<SetCellCommand>(session_->state, value);
    CommandScope guard(*this);
    if (operation_.kind != OperationKind::Save && operation_.kind != OperationKind::Autosave) invalidateOperation();
    session_->history.push(command.release()); // QUndoStack owns the command and calls redo().
    emit stateChanged();
    return result;
}

CommandResult ProjectController::undo() { return moveHistory(true); }
CommandResult ProjectController::redo() { return moveHistory(false); }

CommandResult ProjectController::moveHistory(bool undo)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!session_)
        return CommandResult::rejected(CommandError::NoProject);
    if (!cacheAvailable())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::DatasetUnavailable);
    if (!(undo ? session_->history.canUndo() : session_->history.canRedo()))
        return CommandResult::noChange();
    if (session_->state.inputRevision() == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    CommandScope guard(*this);
    if (operation_.kind != OperationKind::Save && operation_.kind != OperationKind::Autosave) invalidateOperation();
    if (undo)
        session_->history.undo();
    else
        session_->history.redo();
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

std::optional<ImportContext> ProjectController::importContext() const noexcept
{
    if (checkEntry().outcome == CommandOutcome::Rejected || runner_.busy())
        return {};
    return ImportContext{session_ ? std::optional(session_->state.operationStamp()) : std::nullopt,
                         contextEpoch_, lastOperationId_};
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

CommandResult ProjectController::startWork(OperationWork work, OperationKind kind, bool confirmed)
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
    snapshot.contextEpoch = contextEpoch_;
    snapshot.replacementConfirmed = confirmed;
    snapshot.nonZeroZConsumerAvailable = !zConsumer_.isNull();
    CommandScope guard(*this);
    snapshot_ = snapshot;
    operation_ = {};
    operation_.id = ++lastOperationId_;
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

std::unique_ptr<ProjectController::ProjectSession>
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
    auto candidate = std::make_unique<ProjectSession>(
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
                std::unique_ptr<ProjectSession> candidate;
                if (operation_.kind != OperationKind::RecheckCache) {
                    if (lastIdentity_ == std::numeric_limits<std::uint64_t>::max()
                        || contextEpoch_ == std::numeric_limits<std::uint64_t>::max())
                        throw std::overflow_error("project identity/context exhausted");
                    candidate = std::make_unique<ProjectSession>(ProjectState(lastIdentity_ + 1,
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
                        std::unique_ptr<ProjectSession> &candidate;
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
                        std::unique_ptr<ProjectSession> &candidate;
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
    const auto &s = *session_;
    if (s.recovery && persistence::matchesSavedContent(s.state, *s.recovery->state)
        && ((!s.saved && !s.recovery->baseline)
            || (s.saved && s.recovery->baseline && s.saved->manifestPath == s.recovery->baseline->manifestPath
                && s.saved->manifestDigest == s.recovery->baseline->manifestDigest)))
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
            std::move(metadata), s.saved, s.recovery ? s.recovery->digest : QByteArray{}};
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
}

} // namespace cadcontour
