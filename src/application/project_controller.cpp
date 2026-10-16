#include "project_controller.h"
#include "operation_diagnostics.h"

#include <QUndoCommand>

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

ProjectController::ProjectController(QObject *parent) : QObject(parent), runner_(this)
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
    // Drain before history/state/adapters disappear; no event-loop delivery is required.
    shutdown_ = ShutdownState::Closing;
    operation_.publicationRevoked = true;
    runner_.drain();
    history_.clear();
}

const ProjectState *ProjectController::project() const noexcept
{
    return project_ ? &*project_ : nullptr;
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
    if (lastIdentity_ == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::IdentityExhausted);
    CommandScope guard(*this);
    invalidateOperation();
    history_.clear();
    verification_.reset();
    previousVerification_.reset();
    project_.emplace(++lastIdentity_);
    emit stateChanged();
    return CommandResult::applied();
}

CommandResult ProjectController::closeProject()
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!project_)
        return CommandResult::noChange();
    CommandScope guard(*this);
    invalidateOperation();
    history_.clear();
    verification_.reset();
    previousVerification_.reset();
    project_.reset();
    emit stateChanged();
    return CommandResult::applied();
}

CommandResult ProjectController::setCell(double value)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!project_)
        return CommandResult::rejected(CommandError::NoProject);
    const auto result = project_->checkCellChange(value);
    if (result.outcome != CommandOutcome::Applied)
        return result;
    if (history_.index() == std::numeric_limits<int>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::HistoryExhausted);
    auto command = std::make_unique<SetCellCommand>(*project_, value);
    CommandScope guard(*this);
    invalidateOperation();
    history_.push(command.release()); // QUndoStack owns the command and calls redo().
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
    if (!project_)
        return CommandResult::rejected(CommandError::NoProject);
    if (!(undo ? history_.canUndo() : history_.canRedo()))
        return CommandResult::noChange();
    if (project_->inputRevision() == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    CommandScope guard(*this);
    invalidateOperation();
    if (undo)
        history_.undo();
    else
        history_.redo();
    emit stateChanged();
    return CommandResult::applied();
}

CommandResult ProjectController::startOperation(OperationWork work,
                                               std::function<void(VerificationState &)> finalize)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (runner_.busy())
        return CommandResult::rejected(CommandError::Busy);
    if (!project_)
        return CommandResult::rejected(CommandError::NoProject);
    if (lastOperationId_ == std::numeric_limits<OperationId>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::OperationIdExhausted);
    if (!work)
        return CommandResult::rejected(CommandError::InvalidValue);
    const OperationSnapshot snapshot{project_->operationStamp(), project_->cell()};
    CommandScope guard(*this);
    operation_ = {};
    operation_.id = ++lastOperationId_;
    operation_.stamp = snapshot.stamp;
    operation_.status = OperationStatus::Starting;
    operation_.active = true;
    finalize_ = std::move(finalize);
    operationClock_.start();
    operationDiagnostic("start", operation_);
    if (const auto failure = runner_.start(operation_.id, snapshot, std::move(work)))
        finishOperation(operation_.id, WorkerStatus::Failed, *failure, false);
    else
        emit operationChanged();
    return CommandResult::applied(); // acceptance, not successful computation
}

void ProjectController::invalidateOperation()
{
    if (!operation_.active || operation_.decision != PublicationDecision::Pending)
        return;
    operation_.publicationRevoked = true;
    runner_.cancel();
    operationNotificationPending_ = true;
    operationDiagnostic("invalidate", operation_, operationClock_.elapsed());
    // Notification follows settled project state through finishCommand().
}

bool ProjectController::applicable(OperationId id) const noexcept
{
    return shutdown_ == ShutdownState::Open && project_ && operation_.active
        && id == operation_.id && operation_.decision == PublicationDecision::Pending
        && !operation_.publicationRevoked && !operation_.cancellationRequested
        && project_->accepts(operation_.stamp);
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
            const auto &product = *outcome.product;
            if (product.sourceCell != project_->cell())
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
                candidate->acceptedProduct = lease.takeProduct();
                previousVerification_.swap(verification_);
                verification_.swap(candidate); // noexcept owner exchange is the commit
                operation_.decision = PublicationDecision::Accept;
                runner_.decide(id, PublicationDecision::Accept);
                committed = true;
                operationDiagnostic("accept", operation_, operationClock_.elapsed());
            }
        } catch (const std::bad_alloc &) {
            operation_.failure = OperationFailure::make(FailureReason::OutOfMemory, OperationPhase::Finalizing);
        } catch (const std::exception &error) {
            operation_.failure = OperationFailure::make(FailureReason::PublicationFailed,
                                                        OperationPhase::Finalizing, error.what());
        } catch (...) {
            operation_.failure = OperationFailure::make(FailureReason::PublicationFailed, OperationPhase::Finalizing);
        }
    }
    if (committed)
        emit stateChanged(); // observer delivery is outside fallible candidate preparation
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
    return CommandResult::applied();
}

void ProjectController::finishCommand()
{
    if (std::exchange(operationNotificationPending_, false))
        emit operationChanged();
    executing_ = false;
    if (std::exchange(completionPending_, false))
        QTimer::singleShot(0, this, [this] { processReady(operation_.id); });
}

} // namespace cadcontour
