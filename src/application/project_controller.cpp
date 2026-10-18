#include "project_controller.h"
#include "operation_diagnostics.h"

#include <QUndoCommand>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

#include <cassert>
#include <limits>
#include <memory>

namespace cadcontour {
namespace {

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
    : QObject(parent), storageRoot_(QDir::cleanPath(QFileInfo(storageRoot).absoluteFilePath())), runner_(this)
{
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
}

ProjectController::~ProjectController()
{
    disconnect(zConsumerDestroyed_);
    // Drain before history/state/adapters disappear; no event-loop delivery is required.
    shutdown_ = ShutdownState::Closing;
    operation_.publicationRevoked = true;
    runner_.drain();
    session_.reset();
}

const ProjectState *ProjectController::project() const noexcept
{
    return session_ ? &session_->state : nullptr;
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

CommandResult ProjectController::closeProject()
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!session_ && (!operation_.active || operation_.decision != PublicationDecision::Pending))
        return CommandResult::noChange();
    if (contextEpoch_ == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ContextEpochExhausted);
    CommandScope guard(*this);
    invalidateOperation();
    verification_.reset();
    previousVerification_.reset();
    session_.reset();
    ++contextEpoch_;
    emit stateChanged();
    return CommandResult::applied();
}

CommandResult ProjectController::setCell(double value)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!session_)
        return CommandResult::rejected(CommandError::NoProject);
    const auto result = session_->state.checkCellChange(value);
    if (result.outcome != CommandOutcome::Applied)
        return result;
    if (session_->history.index() == std::numeric_limits<int>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::HistoryExhausted);
    auto command = std::make_unique<SetCellCommand>(session_->state, value);
    CommandScope guard(*this);
    invalidateOperation();
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
    if (!(undo ? session_->history.canUndo() : session_->history.canRedo()))
        return CommandResult::noChange();
    if (session_->state.inputRevision() == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    CommandScope guard(*this);
    invalidateOperation();
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
    if (runner_.busy())
        return CommandResult::rejected(CommandError::Busy);
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
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (runner_.busy())
        return CommandResult::rejected(CommandError::Busy);
    if (kind == OperationKind::Verification && !session_)
        return CommandResult::rejected(CommandError::NoProject);
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
    return shutdown_ == ShutdownState::Open && operation_.active
        && id == operation_.id && operation_.decision == PublicationDecision::Pending
        && !operation_.publicationRevoked && !operation_.cancellationRequested
        && snapshot_.contextEpoch == contextEpoch_ && snapshot_.projectContext == current
        && (snapshot_.kind != OperationKind::Import || !snapshot_.projectContext
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
    dataset.storageId = product.storageId.toStdString();
    dataset.storageDirectory = directory.toStdWString();
    dataset.relativeCachePath = relative.toStdWString();
    dataset.sourcePath = product.sourcePath.toStdWString();
    dataset.sourceBytes = product.source.bytesRead;
    dataset.sourceLines = product.source.physicalLines;
    dataset.pointCount = m.pointCount;
    dataset.minimum = {m.bounds.minX, m.bounds.minY};
    dataset.maximum = {m.bounds.maxX, m.bounds.maxY};
    dataset.hasNonZeroZ = m.hasNonZeroZ;
    auto candidate = std::make_unique<ProjectSession>(ProjectState(lastIdentity_ + 1, std::move(dataset)),
        PersistentStorage{directory, relative});
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
    operation_.workerStatus = outcome.status;
    if (outcome.status == WorkerStatus::Failed)
        operation_.failure = outcome.failure;
    bool committed = false;
    if (outcome.status == WorkerStatus::Succeeded && applicable(id)) {
        try {
            if (operation_.kind == OperationKind::Import) {
                if (!outcome.imported || outcome.product)
                    throw std::invalid_argument("expected import product");
                auto candidate = prepareImport(*outcome.imported);
                if (applicable(id)) {
                    struct Commit {
                        ProjectController &controller;
                        std::unique_ptr<ProjectSession> &candidate;
                    } commit{*this, candidate};
                    committed = lease.accept(&commit, [](void *data) noexcept {
                        auto &c = *static_cast<Commit *>(data);
                        c.controller.session_.swap(c.candidate);
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
        emit stateChanged(); // observer delivery is outside fallible candidate preparation
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
    operation_.status = operation_.decision == PublicationDecision::Accept ? OperationStatus::Succeeded
        : operation_.failure ? OperationStatus::Failed
        : operation_.cancellationRequested || status == WorkerStatus::Cancelled ? OperationStatus::Cancelled
        : OperationStatus::Discarded;
    finalize_ = {};
    const bool shutdownReady = shutdown_ == ShutdownState::Closing;
    if (shutdownReady)
        shutdown_ = ShutdownState::Ready;
    operationDiagnostic("released", operation_, operationClock_.elapsed());
    emit operationChanged();
    if (shutdownReady)
        emit shutdownChanged();
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

CommandResult ProjectController::requestShutdown()
{
    if (QThread::currentThread() != thread())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::WrongThread);
    if (shutdown_ != ShutdownState::Open)
        return CommandResult::noChange();
    shutdown_ = ShutdownState::Closing;
    operation_.publicationRevoked = true;
    if (operation_.active && operation_.decision == PublicationDecision::Pending) {
        operation_.cancellationRequested = true;
        operation_.status = OperationStatus::Cancelling;
    }
    runner_.shutdown(); // cancellation, Discard under mutex, unlock, notify before any wait
    operationDiagnostic("shutdown", operation_, operationClock_.isValid() ? operationClock_.elapsed() : 0);
    if (!runner_.busy())
        shutdown_ = ShutdownState::Ready;
    emit operationChanged();
    emit shutdownChanged();
    notifyNonZeroZConfirmation();
    return CommandResult::applied();
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
