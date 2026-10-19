#include "operation_test_support.h"
#include "project_test_access.h"

#include <QFileInfo>
#include <QTemporaryFile>
#include <QTimer>
#include <gtest/gtest.h>

#include <limits>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

using namespace cadcontour;
using namespace cadcontour::test;

TEST(Operations, WorkerSnapshotAndAtomicSinglePublication)
{
    ControlledWork work;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    QThread *publisher = nullptr;
    int published = 0;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        publisher = QThread::currentThread();
        ++published;
        EXPECT_EQ(OperationTestAccess::value(controller), 42);
        EXPECT_EQ(controller.setCell(7).failure->detail, ErrorDetail::ReentrantCommand);
    });
    EXPECT_EQ(OperationTestAccess::start(controller, work.function()).outcome, CommandOutcome::Applied);
    work.entered.wait();
    EXPECT_NE(work.workerThread, controller.thread());
    EXPECT_EQ(work.snapshot.stamp, controller.project()->operationStamp());
    EXPECT_DOUBLE_EQ(work.snapshot.cell, 1);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
    bool guiEvent = false;
    QTimer::singleShot(0, [&] { guiEvent = true; });
    ASSERT_TRUE(spinUntil([&] { return guiEvent; }));
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Succeeded);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
    EXPECT_EQ(publisher, controller.thread());
    EXPECT_EQ(published, 1);
    const auto id = controller.operation().id;
    OperationTestAccess::ready(controller, id);
    OperationTestAccess::poll(controller);
    EXPECT_EQ(published, 1);
    EXPECT_EQ(OperationTestAccess::generation(controller), 1);
}

TEST(Operations, BusyThroughDecisionAndCleanupThenNewIdentity)
{
    Event cleanup, allowCleanup;
    ControlledWork work;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, {},
        [&](PublicationDecision decision) { EXPECT_EQ(decision, PublicationDecision::Accept); cleanup.set(); allowCleanup.wait(); });
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    const auto firstId = controller.operation().id;
    EXPECT_EQ(OperationTestAccess::start(controller, work.function()).failure->category, CommandError::Busy);
    work.complete.set();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return cleanup.isSet(); }));
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.operation().status, OperationStatus::Finishing);
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(OperationTestAccess::start(controller, work.function(99)).failure->category, CommandError::Busy);
    allowCleanup.set();
    ASSERT_TRUE(finish(controller));
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input, 99); });
    ASSERT_TRUE(finish(controller));
    EXPECT_GT(controller.operation().id, firstId);
    EXPECT_EQ(OperationTestAccess::value(controller), 99);
    EXPECT_EQ(OperationTestAccess::previousValue(controller), 42);
    OperationTestAccess::ready(controller, firstId);
    EXPECT_EQ(OperationTestAccess::generation(controller), 2);
}

class StaleInputs : public testing::TestWithParam<int> {};
TEST_P(StaleInputs, NeverPublishesAfterRelevantChange)
{
    ControlledWork work;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    controller.setCell(0.5);
    const auto originalIdentity = controller.project()->runtimeIdentity();
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    switch (GetParam()) {
    case 0: controller.setCell(2); break;
    case 1: controller.setCell(2); controller.setCell(0.5); break;
    case 2: controller.setCell(2); controller.undo(); break;
    case 3: controller.undo(); controller.redo(); break;
    case 4: ProjectControllerTestAccess::newProject(controller); break;
    case 5: controller.closeProject(); break;
    case 6: OperationTestAccess::invalidate(controller); break; // future reset seam
    }
    EXPECT_TRUE(controller.operation().publicationRevoked);
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Discarded);
    EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Succeeded);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
    if (GetParam() == 4)
        EXPECT_GT(controller.project()->runtimeIdentity(), originalIdentity);
    if (GetParam() == 5)
        EXPECT_EQ(controller.project(), nullptr);
}
INSTANTIATE_TEST_SUITE_P(Changes, StaleInputs, testing::Range(0, 7));

TEST(Operations, NoOpAndRejectedCommandsPreserveApplicabilityAndRedo)
{
    ControlledWork work;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    controller.setCell(0.5);
    controller.undo();
    const auto stamp = controller.project()->operationStamp();
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    EXPECT_EQ(controller.setCell(1).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.setCell(-1).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    // A real Redo invalidates; test the preserved branches separately on a fresh launch.
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_FALSE(controller.project()->accepts(stamp));
    controller.undo();
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    EXPECT_EQ(controller.setCell(1).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.setCell(-1).outcome, CommandOutcome::Rejected);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_FALSE(controller.operation().publicationRevoked);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
}

TEST(Operations, CancellationBeforeWorkerEntryDuringWorkAndBeforeCommit)
{
    for (int checkpoint = 0; checkpoint < 3; ++checkpoint) {
        Event nativeEntered, allowEntry;
        ControlledWork work;
        ProjectController controller;
        ProjectControllerTestAccess::newProject(controller);
        if (checkpoint == 0) {
            OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
                [&](std::function<void()> entry) {
                    return std::unique_ptr<QThread>(QThread::create([&, entry = std::move(entry)] {
                        nativeEntered.set(); allowEntry.wait(); entry();
                    }));
                });
        }
        OperationTestAccess::start(controller, work.function());
        if (checkpoint == 0) nativeEntered.wait();
        else work.entered.wait();
        if (checkpoint == 2) {
            work.complete.set();
            auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
            ASSERT_TRUE(waitContext(context, false));
        }
        EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::NoChange);
        EXPECT_TRUE(controller.operation().publicationRevoked);
        allowEntry.set();
        work.complete.set();
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
        EXPECT_EQ(OperationTestAccess::generation(controller), 0);
    }
}

TEST(Operations, CancellationDuringPreparationUsesFinalApplicabilityCheck)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); },
        [&](double &, std::uint64_t &value) { value = 999; controller.cancelOperation(); });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
}

class WorkerFailures : public testing::TestWithParam<int> {};
TEST_P(WorkerFailures, StructuredFailureAndExceptionsKeepPreviousState)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    controller.setCell(0.5);
    controller.undo();
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input, 17); });
    ASSERT_TRUE(finish(controller));
    int changes = 0;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] { ++changes; });
    OperationTestAccess::start(controller, [&](const auto &, const auto &) -> WorkerOutcome {
        switch (GetParam()) {
        case 0: return WorkerOutcome::failed(OperationFailure::make(FailureReason::InvalidProduct, OperationPhase::Computing, "test failure"));
        case 1: throw std::bad_alloc();
        case 2: throw std::runtime_error("test exception");
        case 3: throw 73;
        case 4: return WorkerOutcome::success(nullptr);
        default: return WorkerOutcome::cancelled();
        }
    });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, GetParam() == 5 ? OperationStatus::Cancelled : OperationStatus::Failed);
    EXPECT_EQ(OperationTestAccess::generation(controller), 1);
    EXPECT_EQ(OperationTestAccess::value(controller), 17);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(controller.project()->inputRevision(), 2);
    EXPECT_EQ(changes, 0);
    if (GetParam() == 1) EXPECT_EQ(controller.operation().failure->reason, FailureReason::OutOfMemory);
    if (GetParam() == 2) EXPECT_EQ(controller.operation().failure->reason, FailureReason::StandardException);
    if (GetParam() == 3) EXPECT_EQ(controller.operation().failure->reason, FailureReason::UnknownException);
}
INSTANTIATE_TEST_SUITE_P(Failures, WorkerFailures, testing::Range(0, 6));

class PublicationFailures : public testing::TestWithParam<int> {};
TEST_P(PublicationFailures, CandidateFailureCannotPartiallyChangeSink)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input, 17); });
    ASSERT_TRUE(finish(controller));
    const auto stamp = controller.project()->operationStamp();
    int notifications = 0;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] { ++notifications; });
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input, 23); },
        [&](double &cell, std::uint64_t &value) {
            EXPECT_EQ(OperationTestAccess::value(controller), 17);
            value = 999;
            if (GetParam() == 0) throw std::bad_alloc();
            if (GetParam() == 1) throw std::runtime_error("candidate failed after mutation");
            if (GetParam() == 2) throw 5;
            cell = -1; // fails candidate validation after a non-throwing preparation
        });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Succeeded);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(OperationTestAccess::generation(controller), 1);
    EXPECT_EQ(OperationTestAccess::value(controller), 17);
    EXPECT_TRUE(controller.project()->accepts(stamp));
    EXPECT_EQ(notifications, 0);
}
INSTANTIATE_TEST_SUITE_P(Failures, PublicationFailures, testing::Range(0, 4));

TEST(Operations, ThreadPreparationAndNotStartedInjection)
{
    for (bool skipStart : {false, true}) {
        ProjectController controller;
        ProjectControllerTestAccess::newProject(controller);
        OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), skipStart
            ? std::function<std::unique_ptr<QThread>(std::function<void()>)>{}
            : [](std::function<void()>) -> std::unique_ptr<QThread> { throw std::runtime_error("factory failed"); }, skipStart);
        OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
        EXPECT_EQ(controller.operation().failure->reason, skipStart ? FailureReason::LaunchFailed : FailureReason::PreparationFailed);
        EXPECT_EQ(OperationTestAccess::generation(controller), 0);
    }
}

TEST(Operations, UnconfirmedEntryKeepsResourcesAndBusySlot)
{
    Event nativeEntered, allowEntry;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
        [&](std::function<void()> entry) { return std::unique_ptr<QThread>(QThread::create([&, entry = std::move(entry)] {
            nativeEntered.set(); allowEntry.wait(); entry();
        })); });
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    nativeEntered.wait();
    for (int i = 0; i < 20; ++i) OperationTestAccess::poll(controller);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.operation().status, OperationStatus::Starting);
    EXPECT_EQ(OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); }).failure->category, CommandError::Busy);
    controller.cancelOperation();
    allowEntry.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
}

TEST(Operations, FastCompletionBeforeAnyGuiDelivery)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    Event ready;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, [&] { ready.set(); });
    OperationTestAccess::start(controller, [](const auto &, const auto &) { return WorkerOutcome::cancelled(); });
    auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    // Supply decision without pumping GUI events, then observe stored outcome after native join.
    controller.requestShutdown();
    ready.wait();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Cancelled);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}

TEST(Operations, LatestProgressIsBoundedUnknownAndChangesPhase)
{
    Event burstDone, complete;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    int notifications = 0;
    QObject::connect(&controller, &ProjectController::operationChanged, [&] { ++notifications; });
    OperationTestAccess::start(controller, [&](const auto &input, const WorkerControl &control) {
        for (std::uint64_t i = 0; i < 100000; ++i)
            control.report({OperationPhase::Computing, i, std::nullopt});
        burstDone.set(); complete.wait();
        control.report({OperationPhase::Finalizing, 10, 10});
        return success(input);
    });
    burstDone.wait();
    EXPECT_EQ(notifications, 1); // no queued event per report, even with no event pumping
    OperationTestAccess::poll(controller);
    EXPECT_LE(notifications, 3); // initial, entered, one mailbox read
    EXPECT_EQ(controller.operation().progress.completed, 99999);
    EXPECT_FALSE(controller.operation().progress.total);
    complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().progress.phase, OperationPhase::Finalizing);
    EXPECT_EQ(controller.operation().progress.total, 10);
    const auto settled = notifications;
    for (int i = 0; i < 20; ++i) OperationTestAccess::poll(controller);
    EXPECT_EQ(notifications, settled);
}

TEST(Operations, ReadyDuringReentryIsProcessedAfterCommand)
{
    Event ready;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, [&] { ready.set(); });
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    ready.wait();
    QObject::connect(&controller, &ProjectController::stateChanged, [&] { OperationTestAccess::ready(controller, controller.operation().id); });
    controller.setCell(2); // completion arrives with command guard held; still must be discarded
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Discarded);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}

class TerminalReentry : public testing::TestWithParam<bool> {};
TEST_P(TerminalReentry, RejectsRestartUntilNotificationReturns)
{
    ProjectController controller;
    // These barriers are released before controller drain, including on assertion failure.
    ControlledWork first, second;
    ProjectControllerTestAccess::newProject(controller);
    auto &runner = OperationTestAccess::runner(controller);
    int publications = 0, releases = 0, operationNotifications = 0, shutdownNotifications = 0;
    bool prematureReady = false, terminalObserved = false;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] { ++publications; });
    QObject::connect(&runner, &OperationRunner::released, [&] { ++releases; });
    QObject::connect(&controller, &ProjectController::shutdownChanged, [&] {
        ++shutdownNotifications;
        prematureReady |= controller.shutdownState() == ShutdownState::Ready
            && (runner.busy() || controller.operation().active);
    });
    ASSERT_EQ(OperationTestAccess::start(controller, first.function()).outcome, CommandOutcome::Applied);
    first.entered.wait();
    const auto firstId = controller.operation().id;
    std::optional<CommandResult> restart;
    QObject::connect(&controller, &ProjectController::operationChanged, [&] {
        ++operationNotifications;
        prematureReady |= controller.shutdownState() == ShutdownState::Ready
            && (runner.busy() || controller.operation().active);
        if (terminalObserved || controller.operation().id != firstId || controller.operation().active)
            return;
        terminalObserved = true;
        EXPECT_FALSE(runner.busy());
        EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
        EXPECT_EQ(OperationTestAccess::generation(controller), 1);
        restart = OperationTestAccess::start(controller, second.function(99));
        if (restart->outcome == CommandOutcome::Applied)
            second.entered.wait(); // Original R1: a real second worker is held through old completion.
        if (GetParam()) {
            EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
            EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::NoChange);
        }
    });
    first.complete.set();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return terminalObserved; }));
    ASSERT_TRUE(restart.has_value());
    EXPECT_EQ(restart->outcome, CommandOutcome::Rejected);
    EXPECT_TRUE(restart->failure.has_value());
    if (restart->failure)
        EXPECT_EQ(restart->failure->detail, ErrorDetail::ReentrantCommand);
    EXPECT_EQ(controller.operation().id, firstId);
    EXPECT_FALSE(second.entered.isSet());
    EXPECT_FALSE(controller.operation().active);
    EXPECT_FALSE(runner.busy());
    EXPECT_FALSE(prematureReady);
    EXPECT_EQ(controller.shutdownState(), GetParam() ? ShutdownState::Ready : ShutdownState::Open);
    EXPECT_EQ(shutdownNotifications, GetParam() ? 1 : 0);
    EXPECT_EQ(publications, 1);
    EXPECT_EQ(releases, 1);
    EXPECT_EQ(OperationTestAccess::generation(controller), 1);
    second.complete.set();
    ASSERT_TRUE(finish(controller));
    const auto settledNotifications = operationNotifications;
    for (int i = 0; i < 20; ++i) OperationTestAccess::poll(controller);
    EXPECT_EQ(operationNotifications, settledNotifications);
    EXPECT_EQ(releases, 1);
    if (!GetParam()) {
        ASSERT_EQ(OperationTestAccess::start(controller, second.function(99)).outcome, CommandOutcome::Applied);
        second.entered.wait();
        EXPECT_EQ(controller.operation().id, firstId + 1);
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
        EXPECT_EQ(OperationTestAccess::value(controller), 99);
        EXPECT_EQ(OperationTestAccess::previousValue(controller), 42);
        EXPECT_EQ(OperationTestAccess::generation(controller), 2);
        EXPECT_EQ(publications, 2);
        EXPECT_EQ(releases, 2);
        EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    }
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_FALSE(runner.busy());
    EXPECT_FALSE(prematureReady);
    EXPECT_EQ(shutdownNotifications, 1);
}
INSTANTIATE_TEST_SUITE_P(ShutdownDuringNotification, TerminalReentry, testing::Bool());

TEST(Operations, TerminalNotificationSeesSettledShutdown)
{
    ProjectController controller;
    ControlledWork work;
    ProjectControllerTestAccess::newProject(controller);
    auto &runner = OperationTestAccess::runner(controller);
    ASSERT_EQ(OperationTestAccess::start(controller, work.function()).outcome, CommandOutcome::Applied);
    work.entered.wait();
    ASSERT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.shutdownState(), ShutdownState::Closing);
    int terminalNotifications = 0, shutdownNotifications = 0;
    QObject::connect(&controller, &ProjectController::operationChanged, [&] {
        if (controller.operation().active)
            return;
        ++terminalNotifications;
        EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
        EXPECT_FALSE(runner.busy());
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
        EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::NoChange);
    });
    QObject::connect(&controller, &ProjectController::shutdownChanged, [&] {
        ++shutdownNotifications;
        EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
        EXPECT_FALSE(controller.operation().active);
        EXPECT_FALSE(runner.busy());
    });
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(terminalNotifications, 1);
    EXPECT_EQ(shutdownNotifications, 1);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}

TEST(Operations, ReleaseDuringCommandPreservesOuterGuard)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    auto &runner = OperationTestAccess::runner(controller);
    OperationRunnerTestAccess::configure(runner, {}, true);
    ASSERT_EQ(OperationTestAccess::start(controller, [](const auto &input, const auto &) {
        return success(input);
    }).outcome, CommandOutcome::Applied);
    int notifications = 0;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        OperationTestAccess::poll(controller); // Physical release while setCell owns CommandScope.
        EXPECT_FALSE(controller.operation().active);
        const auto result = controller.setCell(3);
        ASSERT_TRUE(result.failure.has_value());
        EXPECT_EQ(result.failure->detail, ErrorDetail::ReentrantCommand);
    });
    QObject::connect(&controller, &ProjectController::operationChanged, [&] { ++notifications; });
    EXPECT_EQ(controller.setCell(2).outcome, CommandOutcome::Applied);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2);
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_FALSE(runner.busy());
    EXPECT_EQ(notifications, 2); // Terminal completion, then deferred invalidation notification.
}

TEST(Operations, WrongThreadAndIdentityExhaustionRejectBeforeMutation)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    std::thread wrong([&] {
        for (const auto &result : {controller.setCell(2), ProjectControllerTestAccess::newProject(controller), controller.closeProject(),
            controller.undo(), controller.redo(), controller.cancelOperation(), controller.requestShutdown(),
            OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); })})
            EXPECT_EQ(result.failure->detail, ErrorDetail::WrongThread);
    });
    wrong.join();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
    EXPECT_EQ(controller.project()->inputRevision(), 0);
    OperationTestAccess::setLastId(controller, std::numeric_limits<OperationId>::max() - 1);
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().id, std::numeric_limits<OperationId>::max());
    EXPECT_EQ(OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); }).failure->detail, ErrorDetail::OperationIdExhausted);
}

class ShutdownCheckpoints : public testing::TestWithParam<int> {};
TEST_P(ShutdownCheckpoints, DiscardBeforeWaitAndAfterWaitAndDuringWork)
{
    ControlledWork work;
    Event beforeWait, allowWait;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    if (GetParam() == 1)
        OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false,
            [&] { beforeWait.set(); allowWait.wait(); });
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    if (GetParam() != 0) work.complete.set();
    if (GetParam() == 1) beforeWait.wait();
    if (GetParam() == 2) {
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_TRUE(waitContext(context, true));
    }
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    EXPECT_EQ(ProjectControllerTestAccess::newProject(controller).failure->detail, ErrorDetail::ShuttingDown);
    work.complete.set(); allowWait.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}
INSTANTIATE_TEST_SUITE_P(Checkpoints, ShutdownCheckpoints, testing::Range(0, 3));

TEST(Operations, ShutdownDuringPreparationCannotFreeBorrowedProduct)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); },
        [&](double &cell, std::uint64_t &value) {
            controller.requestShutdown();
            cell = 1; value = 999; // product lease remains valid after Discard wakes worker
        });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}

TEST(Operations, DestructorDrainsWithoutEventLoopCallbacks)
{
    for (bool alreadyWaiting : {false, true}) {
        ControlledWork work;
        auto controller = std::make_unique<ProjectController>();
        ProjectControllerTestAccess::newProject(*controller);
        OperationTestAccess::start(*controller, work.function());
        work.entered.wait();
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(*controller));
        if (alreadyWaiting) {
            work.complete.set();
            ASSERT_TRUE(waitContext(context, true));
        }
        std::thread release([&] {
            // wait on the actual shutdown decision rather than elapsed time
            std::unique_lock lock(context->mutex);
            context->decisionChanged.wait(lock, [&] { return context->decision == PublicationDecision::Discard; });
            lock.unlock(); work.complete.set();
        });
        controller.reset(); // blocks safely, never processEvents
        release.join();
        EXPECT_EQ(context->decision, PublicationDecision::Discard);
    }
}

TEST(Operations, ResourcesDiscardOnWorkerAndAcceptedOwnerRetainsProduct)
{
    struct Resource {
        Event &destroyed;
        QThread *&destroyer;
        ~Resource() { destroyer = QThread::currentThread(); destroyed.set(); }
    };
    for (bool accept : {false, true}) {
        Event created, complete, destroyed;
        QThread *destroyer = nullptr, *worker = nullptr;
        QString scratchPath;
        auto controller = std::make_unique<ProjectController>();
        ProjectControllerTestAccess::newProject(*controller);
        OperationTestAccess::start(*controller, [&](const auto &input, const auto &) {
            worker = QThread::currentThread();
            QTemporaryFile scratch;
            if (!scratch.open()) throw std::runtime_error("scratch creation failed");
            scratchPath = scratch.fileName();
            auto result = success(input);
            result.product->resource = std::make_shared<Resource>(destroyed, destroyer);
            created.set(); complete.wait();
            return result;
        });
        created.wait();
        EXPECT_TRUE(QFileInfo::exists(scratchPath));
        if (!accept) controller->cancelOperation();
        complete.set();
        ASSERT_TRUE(finish(*controller));
        EXPECT_FALSE(QFileInfo::exists(scratchPath));
        if (accept) EXPECT_FALSE(destroyed.isSet());
        else { EXPECT_TRUE(destroyed.isSet()); EXPECT_EQ(destroyer, worker); }
        controller.reset();
        EXPECT_TRUE(destroyed.isSet());
    }
}

TEST(Operations, CleanupWarningKeepsAcceptedOutcomeAndSafeRelease)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, {},
        [](PublicationDecision) { throw std::runtime_error("injected cleanup warning"); });
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_TRUE(controller.operation().cleanupWarning);
}

TEST(Operations, Qt612WaitZeroTlsDiscrepancyReproducer)
{
    if (QString::fromLatin1(qVersion()) != QStringLiteral("6.12.0"))
        GTEST_SKIP() << "Installed Qt 6.12.0 discrepancy measurement";
    Event nativeCleanup, allowCleanup, threadFinished;
    struct ExitBarrier {
        Event &entered;
        Event &allow;
        ~ExitBarrier() { entered.set(); allow.wait(); }
    };
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
        [&](std::function<void()> entry) {
            auto thread = std::unique_ptr<QThread>(QThread::create(std::move(entry)));
            QObject::connect(thread.get(), &QThread::finished, thread.get(), [&] { threadFinished.set(); }, Qt::DirectConnection);
            return thread;
        });
    OperationTestAccess::start(controller, [&](const auto &input, const auto &) {
        thread_local std::unique_ptr<ExitBarrier> exitBarrier;
        exitBarrier = std::make_unique<ExitBarrier>(nativeCleanup, allowCleanup);
        return success(input);
    });
    auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_TRUE(waitContext(context, false));
    OperationTestAccess::ready(controller, controller.operation().id);
    nativeCleanup.wait();
    EXPECT_TRUE(threadFinished.isSet());
    const bool joinedWhileTlsBlocked = OperationRunnerTestAccess::waitZero(OperationTestAccess::runner(controller));
    allowCleanup.set();
    ASSERT_TRUE(finish(controller));
    RecordProperty("qt_wait_zero_while_tls_blocked", joinedWhileTlsBlocked ? "true" : "false");
    EXPECT_TRUE(joinedWhileTlsBlocked) << "Qt 6.12.0 discrepancy no longer reproduced; review native guard evidence";
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}

TEST(Operations, NativeThreadCleanupKeepsSlotEvenAfterFinishedSignal)
{
    Event nativeCleanup, allowCleanup, threadFinished;
    struct ExitBarrier {
        Event &entered;
        Event &allow;
        ~ExitBarrier() { entered.set(); allow.wait(); }
    };
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
        [&](std::function<void()> entry) {
            auto thread = std::unique_ptr<QThread>(QThread::create(std::move(entry)));
            QObject::connect(thread.get(), &QThread::finished, thread.get(), [&] { threadFinished.set(); }, Qt::DirectConnection);
            return thread;
        });
    OperationTestAccess::start(controller, [&](const auto &input, const auto &) {
        thread_local std::unique_ptr<ExitBarrier> exitBarrier;
        exitBarrier = std::make_unique<ExitBarrier>(nativeCleanup, allowCleanup);
        return success(input);
    });
    const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    EXPECT_TRUE(waitContext(context, false));
    OperationTestAccess::ready(controller, controller.operation().id);
    nativeCleanup.wait();
    EXPECT_TRUE(threadFinished.isSet());
    EXPECT_FALSE(context->nativeCompletion.finished()); // actual WaitForSingleObject returns TIMEOUT
    OperationTestAccess::poll(controller);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_TRUE(OperationTestAccess::runner(controller).busy());
    EXPECT_EQ(OperationTestAccess::start(controller, {}).failure->category, CommandError::Busy);
    controller.requestShutdown();
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    OperationTestAccess::poll(controller);
    EXPECT_TRUE(controller.operation().active);
    allowCleanup.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}

TEST(Operations, NativeWaitFailureDoesNotReleaseResourcesOrPermitStart)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    auto &runner = OperationTestAccess::runner(controller);
    OperationRunnerTestAccess::configure(runner, {}, false, {}, {}, false, false, true);
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    const auto context = OperationRunnerTestAccess::context(runner);
    EXPECT_TRUE(waitContext(context, false));
    OperationTestAccess::ready(controller, controller.operation().id);
    OperationTestAccess::poll(controller);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_TRUE(runner.busy());
    EXPECT_TRUE(controller.operation().completionObservationError.has_value());
    EXPECT_EQ(OperationTestAccess::start(controller, {}).failure->category, CommandError::Busy);
    controller.requestShutdown();
    OperationTestAccess::poll(controller);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    OperationRunnerTestAccess::allowNativeObservation(context);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
}

TEST(Operations, DuplicateHandleFailureSkipsWorkAndRetainsUntilFallbackHandle)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    auto &runner = OperationTestAccess::runner(controller);
    OperationRunnerTestAccess::configure(runner, {}, false, {}, {}, true, true);
    bool invoked = false;
    OperationTestAccess::start(controller, [&](const auto &input, const auto &) { invoked = true; return success(input); });
    const auto context = OperationRunnerTestAccess::context(runner);
    EXPECT_TRUE(waitContext(context, true));
    OperationTestAccess::poll(controller);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_FALSE(context->nativeCompletion.hasHandle());
    EXPECT_EQ(controller.operation().failure->reason, FailureReason::NativeObservationFailed);
    EXPECT_TRUE(controller.operation().failure->systemCode.has_value());
    controller.requestShutdown();
    OperationTestAccess::poll(controller);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    OperationRunnerTestAccess::allowNativeObservation(context);
    ASSERT_TRUE(finish(controller));
    EXPECT_FALSE(invoked);
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}

TEST(Operations, DestructorFallbackRecoversDuplicateHandleFailureWithoutGuiDelivery)
{
    auto controller = std::make_unique<ProjectController>();
    ProjectControllerTestAccess::newProject(*controller);
    auto &runner = OperationTestAccess::runner(*controller);
    OperationRunnerTestAccess::configure(runner, {}, false, {}, {}, true, true);
    OperationTestAccess::start(*controller, [](const auto &input, const auto &) { return success(input); });
    const auto context = OperationRunnerTestAccess::context(runner);
    EXPECT_TRUE(waitContext(context, true));
    std::thread releaseObservation([context] {
        std::unique_lock lock(context->mutex);
        context->decisionChanged.wait(lock, [&] { return context->decision == PublicationDecision::Discard; });
        OperationRunnerTestAccess::allowNativeObservation(context);
    });
    controller.reset(); // Discard+notify, OpenThread fallback, native wait; no processEvents
    releaseObservation.join();
    EXPECT_TRUE(context->nativeCompletion.finished());
}

TEST(Operations, NativeHandleRemainsValidThroughJoinAndClosesWithRaiiOwner)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_TRUE(finish(controller));
    const auto handle = OperationRunnerTestAccess::nativeHandle(context);
    DWORD flags = 0;
    EXPECT_TRUE(GetHandleInformation(handle, &flags));
    EXPECT_EQ(WaitForSingleObject(handle, 0), WAIT_OBJECT_0);
    context.reset(); // last context owner closes only its independent real handle
    EXPECT_FALSE(GetHandleInformation(handle, &flags));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);
}

TEST(Operations, DestructorWaitsForNativeTlsEvenWhenQtWaitZeroReportsFinished)
{
    Event nativeCleanup, allowCleanup, helperEntered;
    std::atomic<bool> tlsComplete{false};
    struct ExitBarrier {
        Event &entered;
        Event &allow;
        std::atomic<bool> &complete;
        ~ExitBarrier() { entered.set(); allow.wait(); complete.store(true); }
    };
    auto controller = std::make_unique<ProjectController>();
    ProjectControllerTestAccess::newProject(*controller);
    OperationTestAccess::start(*controller, [&](const auto &input, const auto &) {
        thread_local std::unique_ptr<ExitBarrier> exitBarrier;
        exitBarrier = std::make_unique<ExitBarrier>(nativeCleanup, allowCleanup, tlsComplete);
        return success(input);
    });
    const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(*controller));
    EXPECT_TRUE(waitContext(context, false));
    // Wait for actual entry: CreateThread can return before DLL_THREAD_ATTACH.
    // TLS cleanup must not take the loader lock while the helper still needs it.
    std::thread releaseCleanup([&] {
        helperEntered.set();
        nativeCleanup.wait();
        while (!context->cancellation.load(std::memory_order_acquire))
            std::this_thread::yield();
        allowCleanup.set(); // destructor shutdown is now executing; no GUI event delivery
    });
    helperEntered.wait();
    OperationTestAccess::ready(*controller, controller->operation().id);
    nativeCleanup.wait();
    EXPECT_FALSE(tlsComplete.load());
    controller.reset();
    EXPECT_TRUE(tlsComplete.load());
    releaseCleanup.join();
}

TEST(Operations, CooperativeCheckpointAndFailureAfterCancellationStayDistinct)
{
    for (bool failure : {false, true}) {
        ControlledWork work;
        ProjectController controller;
        ProjectControllerTestAccess::newProject(controller);
        const auto controlled = work.function(42, true);
        OperationTestAccess::start(controller, [controlled, failure](const auto &input, const auto &control) {
            const auto cancelled = controlled(input, control);
            return failure ? WorkerOutcome::failed(OperationFailure::make(FailureReason::InvalidProduct,
                                                 OperationPhase::Computing, "failure with cancellation requested"))
                           : WorkerOutcome::cancelled();
        });
        work.entered.wait();
        controller.cancelOperation();
        work.complete.set();
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, failure ? OperationStatus::Failed : OperationStatus::Cancelled);
        EXPECT_EQ(controller.operation().workerStatus, failure ? WorkerStatus::Failed : WorkerStatus::Cancelled);
        EXPECT_EQ(OperationTestAccess::generation(controller), 0);
    }
}

TEST(Operations, DuplicateReadyWhilePendingAndLateNotificationsForOldId)
{
    Event cleanup, releaseCleanup;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, {},
        [&](PublicationDecision) { cleanup.set(); releaseCleanup.wait(); });
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_TRUE(waitContext(context, false));
    const auto oldId = controller.operation().id;
    OperationTestAccess::ready(controller, oldId);
    cleanup.wait();
    OperationTestAccess::ready(controller, oldId);
    EXPECT_EQ(OperationTestAccess::generation(controller), 1);
    releaseCleanup.set();
    ASSERT_TRUE(finish(controller));
    ControlledWork work;
    OperationTestAccess::start(controller, work.function(73));
    work.entered.wait();
    const auto newId = controller.operation().id;
    auto &runner = OperationTestAccess::runner(controller);
    emit runner.progressAvailable(oldId, {OperationPhase::Cleanup, 999, 999});
    emit runner.ready(oldId);
    emit runner.released(oldId, WorkerStatus::Failed, {}, true);
    EXPECT_EQ(controller.operation().id, newId);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_NE(controller.operation().progress.completed, 999);
    EXPECT_FALSE(controller.operation().cleanupWarning);
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(OperationTestAccess::generation(controller), 2);
}

TEST(Operations, ProductReadyIsNotPublishedUntilGateActuallyRuns)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input, 17); });
    ASSERT_TRUE(finish(controller));
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input, 23); });
    auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_TRUE(waitContext(context, true)); // main thread has deliberately not polled the gate
    EXPECT_EQ(OperationTestAccess::value(controller), 17);
    EXPECT_EQ(OperationTestAccess::generation(controller), 1);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Pending);
    OperationTestAccess::ready(controller, controller.operation().id);
    EXPECT_EQ(OperationTestAccess::value(controller), 23);
    ASSERT_TRUE(finish(controller));
}

TEST(Operations, InvalidProgressNeverSuppliesPercentageAndDiagnosticDetailIsBounded)
{
    Event reported, complete;
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    OperationTestAccess::start(controller, [&](const auto &input, const WorkerControl &control) {
        control.report({OperationPhase::Computing, 9, 0});
        control.report({OperationPhase::Computing, 9, 4});
        reported.set(); complete.wait();
        return success(input);
    });
    reported.wait();
    OperationTestAccess::poll(controller);
    EXPECT_FALSE(controller.operation().progress.total);
    EXPECT_EQ(controller.operation().progress.completed, 9);
    complete.set();
    ASSERT_TRUE(finish(controller));
    std::string longDetail(10000, 'x');
    const auto failure = OperationFailure::make(FailureReason::StandardException, OperationPhase::Computing, longDetail.c_str());
    EXPECT_EQ(failure.detail.back(), '\0');
    EXPECT_EQ(std::char_traits<char>::length(failure.detail.data()), 191);
}

TEST(Operations, NoProjectEmptyWorkAndPreparationBadAllocRejectOrFailWithoutMutation)
{
    ProjectController controller;
    EXPECT_EQ(OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); }).failure->category, CommandError::NoProject);
    ProjectControllerTestAccess::newProject(controller);
    EXPECT_EQ(OperationTestAccess::start(controller, {}).failure->category, CommandError::InvalidValue);
    EXPECT_EQ(controller.operation().id, 0);
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
        [](std::function<void()>) -> std::unique_ptr<QThread> { throw std::bad_alloc(); });
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().failure->reason, FailureReason::OutOfMemory);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}
