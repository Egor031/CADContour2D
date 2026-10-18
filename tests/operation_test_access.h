#pragma once

#include "application/project_controller.h"
#include <QFileInfo>

namespace cadcontour {

class OperationRunnerTestAccess {
public:
    static void configure(OperationRunner &runner,
        std::function<std::unique_ptr<QThread>(std::function<void()>)> factory = {},
        bool skipStart = false, std::function<void()> beforeWait = {},
        std::function<void(PublicationDecision)> afterDecision = {},
        bool failDuplicate = false, bool failOpen = false, bool failNativeWait = false)
    {
        auto hooks = std::make_shared<OperationRunner::TestHooks>();
        hooks->factory = std::move(factory);
        hooks->skipStart = skipStart;
        hooks->beforeDecisionWait = std::move(beforeWait);
        hooks->afterDecision = std::move(afterDecision);
        hooks->failDuplicate = failDuplicate;
        hooks->failOpen = failOpen;
        hooks->failNativeWait = failNativeWait;
        runner.nextHooks_ = std::move(hooks);
    }
    static std::shared_ptr<detail::OperationContext> context(OperationRunner &runner) { return runner.context_; }
    static void zHooks(OperationRunner &runner, std::function<void()> beforeRequest = {},
                       std::function<void()> beforeWait = {})
    {
        if (!runner.nextHooks_)
            runner.nextHooks_ = std::make_shared<OperationRunner::TestHooks>();
        runner.nextHooks_->beforeZRequest = std::move(beforeRequest);
        runner.nextHooks_->beforeZWait = std::move(beforeWait);
    }
    static void beforeOwnershipFinalization(OperationRunner &runner,
        std::function<void(PublicationDecision)> hook)
    {
        if (!runner.nextHooks_)
            runner.nextHooks_ = std::make_shared<OperationRunner::TestHooks>();
        runner.nextHooks_->beforeOwnershipFinalization = std::move(hook);
    }
    static bool waitZero(OperationRunner &runner) { return runner.thread_->wait(0); }
    static void *nativeHandle(const std::shared_ptr<detail::OperationContext> &context)
    { return context->nativeCompletion.handle_.load(std::memory_order_acquire); }
    static void allowNativeObservation(const std::shared_ptr<detail::OperationContext> &context)
    {
        context->nativeCompletion.failOpen_.store(false);
        context->nativeCompletion.failWait_.store(false);
    }
};

class OperationTestAccess {
public:
    static CommandResult import(ProjectController &controller, ImportRequest request, io::ZHandler handler)
    { return controller.startImport(std::move(request), std::move(handler)); }
    static void preparation(ProjectController &controller, std::function<void()> hook)
    { controller.prepareImportHook_ = std::move(hook); }
    static CommandResult importWork(ProjectController &controller, ImportRequest request,
                                   std::function<void(WorkerOutcome &)> transform)
    {
        request.sourcePath = QFileInfo(request.sourcePath).absoluteFilePath();
        controller.importSourcePath_ = request.sourcePath;
        auto work = ascXyzImportWork(request, controller.storageRoot_);
        return controller.startWork([work = std::move(work), transform = std::move(transform)]
            (const auto &snapshot, const auto &control) mutable {
                auto outcome = work(snapshot, control);
                transform(outcome);
                return outcome;
            }, OperationKind::Import, request.replacementConfirmed);
    }
    static CommandResult start(ProjectController &controller, OperationWork work,
                               std::function<void(double &, std::uint64_t &)> finalize = {})
    {
        return controller.startOperation(std::move(work), finalize
            ? std::function<void(ProjectController::VerificationState &)>(
                [finalize = std::move(finalize)](auto &candidate) {
                    finalize(candidate.sourceCell, candidate.value);
                }) : std::function<void(ProjectController::VerificationState &)>{});
    }
    static void poll(ProjectController &controller) { controller.runner_.poll(); }
    static OperationRunner &runner(ProjectController &controller) { return controller.runner_; }
    static std::uint64_t generation(const ProjectController &controller)
    { return controller.verification_ ? controller.verification_->generation : 0; }
    static std::uint64_t value(const ProjectController &controller)
    { return controller.verification_ ? controller.verification_->value : 0; }
    static std::uint64_t previousValue(const ProjectController &controller)
    { return controller.previousVerification_ ? controller.previousVerification_->value : 0; }
    static void setLastId(ProjectController &controller, OperationId id) { controller.lastOperationId_ = id; }
    static void ready(ProjectController &controller, OperationId id) { controller.processReady(id); }
    static void invalidate(ProjectController &controller) { controller.invalidateOperation(); }
};

} // namespace cadcontour
