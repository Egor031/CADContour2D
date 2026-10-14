#include "operation_runner.h"

#include <QScopedValueRollback>

#include <cassert>
#include <exception>
#include <new>
#include <utility>

namespace cadcontour {

bool WorkerControl::cancelled() const noexcept
{
    return context_->cancellation.load(std::memory_order_acquire);
}
void WorkerControl::report(OperationProgress progress) const
{
    // Invalid totals are shown as unknown, never as a fictitious percentage.
    if (progress.total && (*progress.total == 0 || progress.completed > *progress.total))
        progress.total.reset();
    std::lock_guard lock(context_->mutex);
    context_->progress = progress;
    context_->progressDirty = true;
}

ReadyProductLease::ReadyProductLease(ReadyProductLease &&other) noexcept
    : context_(std::move(other.context_)) {}
ReadyProductLease::~ReadyProductLease()
{
    if (!context_)
        return;
    {
        std::lock_guard lock(context_->mutex);
        context_->leased = false;
    }
    context_->decisionChanged.notify_all();
}
std::unique_ptr<VerificationProduct> ReadyProductLease::takeProduct() noexcept
{
    // Worker cannot touch outcome while leased. Owner-thread access only.
    return std::move(context_->outcome.product);
}

OperationRunner::OperationRunner(QObject *parent) : QObject(parent)
{
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &OperationRunner::poll);
}
OperationRunner::~OperationRunner() { drain(); }

std::optional<OperationFailure> OperationRunner::start(OperationId id, OperationSnapshot snapshot,
                                                       OperationWork work)
{
    assert(QThread::currentThread() == QObject::thread());
    assert(!busy());
    const auto hooks = std::exchange(nextHooks_, {});
    try {
        auto context = std::make_shared<detail::OperationContext>();
        if (hooks) {
            context->nativeCompletion.failDuplicate_.store(hooks->failDuplicate);
            context->nativeCompletion.failOpen_.store(hooks->failOpen);
            context->nativeCompletion.failWait_.store(hooks->failNativeWait);
        }
        std::function<void()> entry = [context, snapshot, work = std::move(work), hooks]() mutable {
            execute(context, snapshot, work, hooks);
        };
        auto thread = hooks && hooks->factory ? hooks->factory(std::move(entry))
                                            : std::unique_ptr<QThread>(QThread::create(std::move(entry)));
        if (!thread)
            return OperationFailure::make(FailureReason::PreparationFailed, OperationPhase::Preparing);
        context_ = std::move(context);
        thread_ = std::move(thread);
        id_ = id;
        enteredReported_ = false;
        reportedObservationError_ = 0;
        // QThread::start returns void. No timeout/signal is used as proof of failure.
        if (!(hooks && hooks->skipStart))
            thread_->start();
        timer_.start();
        return std::nullopt;
    } catch (const std::bad_alloc &) {
        if (context_) { shutdown(); timer_.start(); return std::nullopt; }
        return OperationFailure::make(FailureReason::OutOfMemory, OperationPhase::Preparing);
    } catch (const std::exception &error) {
        if (context_) { shutdown(); timer_.start(); return std::nullopt; }
        return OperationFailure::make(FailureReason::PreparationFailed, OperationPhase::Preparing, error.what());
    } catch (...) {
        if (context_) { shutdown(); timer_.start(); return std::nullopt; }
        return OperationFailure::make(FailureReason::PreparationFailed, OperationPhase::Preparing);
    }
}

void OperationRunner::execute(const std::shared_ptr<detail::OperationContext> &context,
                              const OperationSnapshot &snapshot, OperationWork &work,
                              const std::shared_ptr<TestHooks> &hooks) noexcept
{
    context->workerEntered.store(true, std::memory_order_release); // first wrapper action
    WorkerOutcome outcome;
    OperationPhase phase = OperationPhase::Computing;
    try {
        const WorkerControl control(context);
        if (!context->nativeCompletion.captureCurrent()) {
            auto failure = OperationFailure::make(FailureReason::NativeObservationFailed,
                OperationPhase::Preparing, "DuplicateHandle failed; operation was not executed");
            failure.systemCode = context->nativeCompletion.error();
            outcome = WorkerOutcome::failed(failure);
        } else {
            outcome = control.cancelled() ? WorkerOutcome::cancelled() : work(snapshot, control);
        }
        if (outcome.status == WorkerStatus::Succeeded && !outcome.product)
            outcome = WorkerOutcome::failed(OperationFailure::make(FailureReason::InvalidProduct, phase));
    } catch (const std::bad_alloc &) {
        outcome = WorkerOutcome::failed(OperationFailure::make(FailureReason::OutOfMemory, phase));
    } catch (const std::exception &error) {
        outcome = WorkerOutcome::failed(OperationFailure::make(FailureReason::StandardException, phase, error.what()));
    } catch (...) {
        outcome = WorkerOutcome::failed(OperationFailure::make(FailureReason::UnknownException, phase));
    }
    {
        std::lock_guard lock(context->mutex);
        context->outcome = std::move(outcome);
        context->ready = true;
        // A Discard already supplied by shutdown is deliberately preserved.
    }
    context->decisionChanged.notify_all();
    try {
        if (hooks && hooks->beforeDecisionWait)
            hooks->beforeDecisionWait();
    } catch (...) {
        std::lock_guard lock(context->mutex);
        context->cleanupWarning = true;
    }
    PublicationDecision decision;
    {
        std::unique_lock lock(context->mutex);
        context->waitingForDecision = true;
        context->decisionChanged.notify_all();
        context->decisionChanged.wait(lock, [&] {
            return context->decision != PublicationDecision::Pending && !context->leased
                   && context->nativeCompletion.hasHandle();
        });
        context->waitingForDecision = false;
        decision = context->decision;
    }
    // Product destruction and all scratch/capture cleanup run outside the mutex, on worker.
    context->outcome.product.reset();
    try {
        if (hooks && hooks->afterDecision)
            hooks->afterDecision(decision);
    } catch (...) {
        std::lock_guard lock(context->mutex);
        context->cleanupWarning = true;
    }
    work = {}; // release worker-owned immutable handles before confirming native completion
}

void OperationRunner::cancel() noexcept
{
    if (context_)
        context_->cancellation.store(true, std::memory_order_release);
}
void OperationRunner::shutdown() noexcept
{
    cancel();
    if (context_)
        decide(id_, PublicationDecision::Discard); // Pending only, never overwrites Accept
}
bool OperationRunner::decide(OperationId id, PublicationDecision decision) noexcept
{
    if (!context_ || id != id_ || decision == PublicationDecision::Pending)
        return false;
    bool changed = false;
    {
        std::lock_guard lock(context_->mutex);
        if (context_->decision == PublicationDecision::Pending) {
            context_->decision = decision;
            changed = true;
        }
    }
    context_->decisionChanged.notify_all();
    return changed;
}
ReadyProductLease OperationRunner::leaseReady(OperationId id)
{
    if (!context_ || id != id_)
        return {};
    std::lock_guard lock(context_->mutex);
    if (!context_->ready || context_->leased || context_->decision != PublicationDecision::Pending)
        return {};
    context_->leased = true;
    return ReadyProductLease(context_);
}

void OperationRunner::poll()
{
    assert(QThread::currentThread() == QObject::thread());
    if (!context_ || polling_)
        return;
    QScopedValueRollback guard(polling_, true);
    const auto context = context_;
    const auto id = id_;
    if (context->workerEntered.load(std::memory_order_acquire)
        && context->nativeCompletion.captureFromOwner())
        context->decisionChanged.notify_all();
    if (!enteredReported_ && context->workerEntered.load(std::memory_order_acquire)) {
        enteredReported_ = true;
        emit entered(id);
    }
    OperationProgress progress;
    bool dirty, ready;
    {
        std::lock_guard lock(context->mutex);
        progress = context->progress;
        dirty = std::exchange(context->progressDirty, false);
        ready = context->ready && context->decision == PublicationDecision::Pending;
    }
    if (dirty)
        emit progressAvailable(id, progress);
    if (ready)
        emit this->ready(id); // if controller is busy, mailbox remains pending for next poll
    if (context->workerEntered.load(std::memory_order_acquire)
        && !context->nativeCompletion.finished()) {
        const auto error = context->nativeCompletion.error();
        if (error && error != reportedObservationError_) {
            reportedObservationError_ = error;
            emit observationFailed(id, error);
        }
        return;
    }
    if (!thread_->wait(0))
        return; // includes launch not yet confirmed and native cleanup still in progress
    WorkerStatus status;
    OperationFailure failure;
    bool warning;
    {
        std::lock_guard lock(context->mutex);
        status = context->outcome.status;
        failure = context->outcome.failure;
        warning = context->cleanupWarning;
    }
    if (!context->workerEntered.load(std::memory_order_acquire)) {
        status = WorkerStatus::Failed;
        failure = OperationFailure::make(FailureReason::LaunchFailed, OperationPhase::Preparing,
                                        "worker did not enter execution");
    }
    timer_.stop();
    thread_.reset(); // both native completion and Qt join confirmed before destruction
    context_.reset();
    emit released(id, status, failure, warning);
}

void OperationRunner::drain() noexcept
{
    shutdown(); // Discard -> notify -> wait, even without a running event loop
    timer_.stop();
    disconnect(this, nullptr, nullptr, nullptr);
    if (thread_) {
        // A delayed entry or a failed DuplicateHandle must also be drainable without
        // GUI delivery. Keep Discard posted while retaining a fallback real handle.
        while (!thread_->wait(0)) {
            if (context_->workerEntered.load(std::memory_order_acquire)
                && context_->nativeCompletion.captureFromOwner())
                context_->decisionChanged.notify_all();
            QThread::msleep(10);
        }
        if (context_->workerEntered.load(std::memory_order_acquire)) {
            while (!context_->nativeCompletion.captureFromOwner())
                QThread::msleep(100); // never release unconfirmed live resources
            context_->decisionChanged.notify_all();
            while (!context_->nativeCompletion.wait())
                QThread::msleep(100);
        }
    }
    thread_.reset();
    context_.reset();
}

} // namespace cadcontour
