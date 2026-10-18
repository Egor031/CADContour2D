#include "operation_test_support.h"
#include "project_test_access.h"
#include "cache/cache_reader.h"

#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <thread>
#include <vector>

using namespace cadcontour;
using namespace cadcontour::test;

namespace {
class ZConfirmation : public testing::Test {
protected:
    QTemporaryDir area;
    QString root = area.filePath("storage");
    QString source = area.filePath("source.xyz");
    QObject consumer;
    ProjectController controller{root};
    cache::ResourceLimits limits{8ULL << 20, 64ULL << 20, 8};

    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        ASSERT_EQ(controller.setNonZeroZConsumer(&consumer).outcome, CommandOutcome::Applied);
        write("1 2 3\n4 5 0\n6 7 -9\n");
    }
    void write(const QByteArray &bytes)
    {
        QFile file(source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }
    ImportRequest request(bool replace = false) const { return {source, limits, replace}; }
    void start(bool replace = false)
    { ASSERT_EQ(controller.importAscXyz(request(replace)).outcome, CommandOutcome::Applied); }
    auto context() { return OperationRunnerTestAccess::context(OperationTestAccess::runner(controller)); }
    bool waiting()
    {
        auto c = context();
        std::unique_lock lock(c->mutex);
        return c->decisionChanged.wait_for(lock, std::chrono::seconds(10), [&] { return c->waitingForZ; });
    }
    NonZeroZRequest pending()
    {
        EXPECT_TRUE(waiting());
        auto value = controller.pendingNonZeroZ();
        EXPECT_TRUE(value);
        return value.value_or(NonZeroZRequest{});
    }
    int attempts() const { return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(); }
    void cancelled()
    {
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
        EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Cancelled);
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_FALSE(controller.operation().failure);
        EXPECT_FALSE(controller.pendingNonZeroZ());
    }
    void initial()
    {
        write("1 2 0\n4 5 0\n6 7 0\n");
        start();
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        ASSERT_NE(controller.project(), nullptr);
        controller.setCell(2);
        controller.setCell(3);
        controller.undo();
        ASSERT_TRUE(controller.canUndo());
        ASSERT_TRUE(controller.canRedo());
        write("1 2 3\n4 5 0\n6 7 -9\n");
    }
    void readCache(std::uint64_t count, bool nonzero = true)
    {
        ASSERT_NE(controller.project(), nullptr);
        const auto &dataset = *controller.project()->dataset();
        EXPECT_EQ(dataset.pointCount, count);
        EXPECT_EQ(dataset.hasNonZeroZ, nonzero);
        auto opened = cache::CacheReader::open(QString::fromStdWString(dataset.cachePath().wstring()), limits);
        ASSERT_TRUE(opened);
        EXPECT_EQ(opened.value()->metadata().id, dataset.cacheId);
        EXPECT_EQ(opened.value()->metadata().hasNonZeroZ, nonzero);
        std::vector<bool> seen(static_cast<std::size_t>(count));
        auto scanned = opened.value()->scanAll([&](auto batch) {
            for (const auto &record : batch) {
                EXPECT_LT(record.id, count);
                if (record.id >= count) continue;
                EXPECT_FALSE(seen[record.id]);
                seen[record.id] = true;
                EXPECT_EQ(std::bit_cast<std::uint64_t>(record.point.x),
                          std::bit_cast<std::uint64_t>(double(record.id + 1)));
                EXPECT_EQ(std::bit_cast<std::uint64_t>(record.point.y),
                          std::bit_cast<std::uint64_t>(-double(record.id + 1)));
            }
            return cache::success();
        });
        ASSERT_TRUE(scanned);
        EXPECT_EQ(scanned.value().returnedRecords, count);
        for (bool found : seen) EXPECT_TRUE(found);
        ASSERT_TRUE(opened.value()->validate(limits, area.path()));
    }
};
} // namespace

TEST_F(ZConfirmation, FlatAndSignedZerosDoNotRequest)
{
    write("1 -1 +0\n2 -2 -0\n3 -3 0.0\n");
    int requests = 0;
    QObject::connect(&controller, &ProjectController::nonZeroZConfirmationChanged, [&] {
        if (controller.pendingNonZeroZ()) ++requests;
    });
    start();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(requests, 0);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    readCache(3, false);
}

TEST_F(ZConfirmation, CancelWakesZWaitWithoutPollShutdownOrDrain)
{
    start();
    ASSERT_TRUE(waiting()); // observed under the same mutex after the worker releases it in wait
    auto c = context();
    ASSERT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
    // No GUI events/poll/shutdown/drain until the worker itself publishes Ready.
    ASSERT_TRUE(waitContext(c, false));
    {
        std::lock_guard lock(c->mutex);
        EXPECT_FALSE(c->waitingForZ);
        EXPECT_EQ(c->outcome.status, WorkerStatus::Cancelled);
        EXPECT_FALSE(c->zRequest);
    }
    EXPECT_EQ(controller.project(), nullptr);
    cancelled(); // only now supply Discard and verify final cleanup
    EXPECT_EQ(attempts(), 0);
}

class ZPosition : public ZConfirmation, public testing::WithParamInterface<int> {};
TEST_P(ZPosition, ContinueImportsEveryRecordOnceAndOnlyRequestsFirstNonzero)
{
    // Multiple read blocks and Writer batches; physical lines include blank lines.
    constexpr int count = 10000;
    const int first = GetParam();
    QByteArray bytes("\n");
    for (int i = 1; i <= count; ++i)
        bytes += QByteArray::number(i) + " " + QByteArray::number(-i)
            + (i >= first ? " 5\n" : " -0\n");
    write(bytes);
    int requests = 0;
    QObject::connect(&controller, &ProjectController::nonZeroZConfirmationChanged, [&] {
        EXPECT_EQ(QThread::currentThread(), controller.thread());
        if (const auto z = controller.pendingNonZeroZ()) {
            ++requests;
            EXPECT_EQ(z->operationId, controller.operation().id);
            EXPECT_FALSE(z->requestId.isNull());
            EXPECT_EQ(z->line, first + 1);
            EXPECT_EQ(z->value, 5);
            EXPECT_EQ(controller.operation().decision, PublicationDecision::Pending);
            EXPECT_EQ(controller.project(), nullptr);
            EXPECT_EQ(controller.answerNonZeroZ(*z, io::ZDecision::Continue).outcome, CommandOutcome::Applied);
        }
    });
    start();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(requests, 1);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
    EXPECT_EQ(controller.project()->dataset()->sourceLines, count + 1);
    EXPECT_EQ(controller.project()->dataset()->sourceBytes, bytes.size());
    readCache(count);
}
INSTANTIATE_TEST_SUITE_P(FirstMiddleLast, ZPosition, testing::Values(1, 5000, 10000));

TEST_F(ZConfirmation, SubnormalRequiresDecision)
{
    write("1 -1 4.9406564584124654e-324\n2 -2 -0\n");
    start();
    const auto z = pending();
    EXPECT_EQ(std::bit_cast<std::uint64_t>(z.value), 1);
    EXPECT_EQ(z.line, 1);
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    readCache(2);
}

TEST_F(ZConfirmation, UserCancelDoesNotCreateProject)
{
    start();
    const auto z = pending();
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Cancel).outcome, CommandOutcome::Applied);
    cancelled();
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ZConfirmation, UserCancelReplacementPreservesSessionAndBothHistoryBranches)
{
    initial();
    const auto *previous = controller.project();
    const auto stamp = previous->operationStamp();
    start(true);
    const auto z = pending();
    EXPECT_EQ(controller.project(), previous);
    controller.answerNonZeroZ(z, io::ZDecision::Cancel);
    cancelled();
    EXPECT_EQ(controller.project(), previous);
    EXPECT_EQ(previous->operationStamp(), stamp);
    EXPECT_TRUE(controller.canUndo());
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(previous->cell(), 3);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(previous->cell(), 2);
    EXPECT_EQ(attempts(), 1);
}

TEST_F(ZConfirmation, MissingConsumerFailsClosedWithDiagnostic)
{
    EXPECT_EQ(controller.setNonZeroZConsumer(nullptr).outcome, CommandOutcome::Applied);
    start();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_FALSE(controller.pendingNonZeroZ());
    ASSERT_TRUE(controller.operation().failure);
    ASSERT_TRUE(controller.operation().failure->importError);
    EXPECT_EQ(controller.operation().failure->importError->code, cache::ErrorCode::InvalidInput);
    EXPECT_TRUE(controller.operation().failure->importError->detail.contains("decision handler"));
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ZConfirmation, GeneralCancelBeforeRequestPublication)
{
    Event entered, release;
    OperationRunnerTestAccess::zHooks(OperationTestAccess::runner(controller), [&] { entered.set(); release.wait(); });
    start();
    entered.wait();
    EXPECT_FALSE(controller.pendingNonZeroZ());
    controller.cancelOperation();
    release.set();
    cancelled();
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ZConfirmation, GeneralCancelAfterPublicationBeforeWaitCannotLoseWakeup)
{
    Event published, release;
    OperationRunnerTestAccess::zHooks(OperationTestAccess::runner(controller), {}, [&] { published.set(); release.wait(); });
    start();
    published.wait();
    const auto z = controller.pendingNonZeroZ();
    ASSERT_TRUE(z);
    controller.cancelOperation();
    EXPECT_EQ(controller.answerNonZeroZ(*z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    release.set();
    cancelled();
}

TEST_F(ZConfirmation, GeneralCancelDuringWaitAndLateContinue)
{
    start();
    const auto z = pending();
    controller.cancelOperation();
    EXPECT_FALSE(controller.pendingNonZeroZ());
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    cancelled();
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ZConfirmation, ContinueThenGeneralCancelBeforeWorkerResumesRevokesPublication)
{
    Event published, release;
    OperationRunnerTestAccess::zHooks(OperationTestAccess::runner(controller), {}, [&] { published.set(); release.wait(); });
    start();
    published.wait();
    const auto z = *controller.pendingNonZeroZ();
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Applied);
    controller.cancelOperation();
    release.set();
    cancelled();
    EXPECT_TRUE(controller.operation().publicationRevoked);
    EXPECT_EQ(controller.project(), nullptr);
}

TEST_F(ZConfirmation, DuplicateInvalidAndWrongIdentityResponsesAreRejected)
{
    Event published, release;
    OperationRunnerTestAccess::zHooks(OperationTestAccess::runner(controller), {}, [&] { published.set(); release.wait(); });
    start();
    published.wait();
    const auto z = *controller.pendingNonZeroZ();
    auto wrong = z;
    ++wrong.operationId;
    EXPECT_EQ(controller.answerNonZeroZ(wrong, io::ZDecision::Continue).failure->category, CommandError::InvalidValue);
    wrong = z;
    wrong.requestId = QUuid::createUuid();
    EXPECT_EQ(controller.answerNonZeroZ(wrong, io::ZDecision::Continue).failure->category, CommandError::InvalidValue);
    EXPECT_EQ(controller.answerNonZeroZ(z, static_cast<io::ZDecision>(99)).failure->category, CommandError::InvalidValue);
    EXPECT_EQ(controller.pendingNonZeroZ(), z);
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Applied);
    for (const auto decision : {io::ZDecision::Continue, io::ZDecision::Cancel})
        EXPECT_EQ(controller.answerNonZeroZ(z, decision).outcome, CommandOutcome::Rejected);
    release.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Cancel).outcome, CommandOutcome::Rejected);
}

TEST_F(ZConfirmation, UserCancelThenContinueCannotResume)
{
    start();
    const auto z = pending();
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Cancel).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    cancelled();
}

TEST_F(ZConfirmation, ShutdownDuringWaitAndLateAnswer)
{
    start();
    const auto z = pending();
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).failure->detail, ErrorDetail::ShuttingDown);
    cancelled();
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(attempts(), 0);
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).failure->detail, ErrorDetail::ShuttingDown);
}

TEST_F(ZConfirmation, CloseDuringFirstImportAlsoRevokesNoProjectContext)
{
    start();
    const auto z = pending();
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_TRUE(controller.operation().publicationRevoked);
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    cancelled();
    EXPECT_EQ(controller.project(), nullptr);
}

TEST_F(ZConfirmation, RelevantInputChangeAndContextReplacementWakeWorker)
{
    initial();
    for (int mode = 0; mode != 4; ++mode) {
        start(true);
        const auto z = pending();
        if (mode == 0) controller.setCell(4);
        if (mode == 1) { controller.undo(); controller.redo(); } // revision ABA cannot restore applicability
        if (mode == 2) ProjectControllerTestAccess::newProject(controller);
        if (mode == 3) controller.closeProject();
        EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
        cancelled();
        EXPECT_TRUE(controller.operation().publicationRevoked);
    }
    EXPECT_EQ(controller.project(), nullptr);
}

TEST_F(ZConfirmation, OldOperationAndOldControllerTokensCannotAnswerNewRequest)
{
    start();
    const auto old = pending();
    controller.cancelOperation();
    cancelled();
    start();
    const auto current = pending();
    EXPECT_NE(old.operationId, current.operationId);
    EXPECT_EQ(controller.answerNonZeroZ(old, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    auto forged = old;
    forged.operationId = current.operationId;
    EXPECT_EQ(controller.answerNonZeroZ(forged, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    controller.answerNonZeroZ(current, io::ZDecision::Cancel);
    cancelled();

    ProjectController another(root);
    another.setNonZeroZConsumer(&consumer);
    another.importAscXyz(request()); // OperationId starts at 1 again
    auto c = OperationRunnerTestAccess::context(OperationTestAccess::runner(another));
    {
        std::unique_lock lock(c->mutex);
        ASSERT_TRUE(c->decisionChanged.wait_for(lock, std::chrono::seconds(10), [&] { return c->waitingForZ; }));
    }
    ASSERT_EQ(another.pendingNonZeroZ()->operationId, old.operationId);
    EXPECT_NE(another.pendingNonZeroZ()->requestId, old.requestId);
    EXPECT_EQ(another.answerNonZeroZ(old, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    another.cancelOperation();
    ASSERT_TRUE(finish(another));
}

TEST_F(ZConfirmation, BusyUntilNativeCompletionEvenAfterConfirmationCancel)
{
    Event finalizing, release;
    OperationRunnerTestAccess::beforeOwnershipFinalization(OperationTestAccess::runner(controller), [&](auto decision) {
        EXPECT_EQ(decision, PublicationDecision::Discard);
        finalizing.set(); release.wait();
    });
    start();
    const auto z = pending();
    EXPECT_EQ(controller.importAscXyz(request()).failure->category, CommandError::Busy);
    controller.answerNonZeroZ(z, io::ZDecision::Cancel);
    ASSERT_TRUE(waitContext(context(), false));
    OperationTestAccess::poll(controller);
    finalizing.wait();
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.importAscXyz(request()).failure->category, CommandError::Busy);
    release.set();
    cancelled();
}

TEST_F(ZConfirmation, ConsumerDestructionAndExplicitDetachCancelWait)
{
    for (bool destroy : {false, true}) {
        auto owner = std::make_unique<QObject>();
        controller.setNonZeroZConsumer(owner.get());
        start();
        const auto z = pending();
        if (destroy) owner.reset();
        else EXPECT_EQ(controller.setNonZeroZConsumer(nullptr).outcome, CommandOutcome::Applied);
        EXPECT_FALSE(controller.pendingNonZeroZ());
        EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
        cancelled();
        EXPECT_EQ(attempts(), 0);
    }
}

TEST_F(ZConfirmation, ControllerDestructorDrainsWaitingImportWithoutEventProcessing)
{
    auto owner = std::make_unique<ProjectController>(root);
    owner->setNonZeroZConsumer(&consumer);
    owner->importAscXyz(request());
    const auto c = OperationRunnerTestAccess::context(OperationTestAccess::runner(*owner));
    {
        std::unique_lock lock(c->mutex);
        ASSERT_TRUE(c->decisionChanged.wait_for(lock, std::chrono::seconds(10), [&] { return c->waitingForZ; }));
    }
    owner.reset(); // no poll/processEvents since launch, no response or GUI dependency
    EXPECT_TRUE(c->nativeCompletion.finished());
    EXPECT_EQ(c->outcome.status, WorkerStatus::Cancelled);
    EXPECT_EQ(c->decision, PublicationDecision::Discard);
    EXPECT_FALSE(c->zRequest);
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ZConfirmation, ContinueStillRequiresReadyPublicationGate)
{
    start();
    controller.answerNonZeroZ(pending(), io::ZDecision::Continue);
    ASSERT_TRUE(waitContext(context(), false));
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Pending);
    controller.cancelOperation(); // completed import cannot publish after irrevocable cancellation
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ZConfirmation, WrongThreadAndReentrantCommandsCannotAnswer)
{
    start();
    const auto z = pending();
    std::thread other([&] {
        EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).failure->detail, ErrorDetail::WrongThread);
        EXPECT_EQ(controller.setNonZeroZConsumer(nullptr).failure->detail, ErrorDetail::WrongThread);
        EXPECT_FALSE(controller.pendingNonZeroZ());
    });
    other.join();
    CommandResult nested = CommandResult::applied();
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        nested = controller.answerNonZeroZ(z, io::ZDecision::Continue);
    });
    ProjectControllerTestAccess::newProject(controller);
    EXPECT_EQ(nested.failure->detail, ErrorDetail::ReentrantCommand);
    cancelled();
}

TEST_F(ZConfirmation, ConsumerRegistrationChecksThreadBusyAndReentry)
{
    EXPECT_EQ(controller.setNonZeroZConsumer(&consumer).outcome, CommandOutcome::NoChange);
    QObject replacement;
    start();
    pending();
    EXPECT_EQ(controller.setNonZeroZConsumer(&replacement).failure->category, CommandError::Busy);
    QObject foreign;
    QThread other;
    foreign.moveToThread(&other);
    EXPECT_EQ(controller.setNonZeroZConsumer(&foreign).failure->detail, ErrorDetail::WrongThread);
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        EXPECT_EQ(controller.setNonZeroZConsumer(nullptr).failure->detail, ErrorDetail::ReentrantCommand);
    });
    ProjectControllerTestAccess::newProject(controller);
    cancelled();
}

TEST_F(ZConfirmation, GuardedStateObserversRejectResponse)
{
    initial();
    start(true);
    const auto z = pending();
    CommandResult response = CommandResult::applied();
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        response = controller.answerNonZeroZ(z, io::ZDecision::Continue);
    });
    controller.setCell(4);
    EXPECT_EQ(response.failure->detail, ErrorDetail::ReentrantCommand);
    cancelled();
}

TEST_F(ZConfirmation, ParserAndWriterStayPausedAndContinueWithoutSourceRewind)
{
    struct Counters {
        std::atomic<std::uint64_t> reads{0}, seeks{0}, measurements{0};
    } counters;
    class ObservedFile final : public QFile {
    public:
        ObservedFile(const QString &path, Counters &c) : QFile(path), counters(c) {}
        bool seek(qint64 position) override { ++counters.seeks; return QFile::seek(position); }
        qint64 readData(char *data, qint64 maximum) override
        { ++counters.reads; return QFile::readData(data, maximum); }
        Counters &counters;
    };
    QByteArray bytes;
    for (int i = 1; i <= 12000; ++i)
        bytes += QByteArray::number(i) + " " + QByteArray::number(-i) + (i >= 6000 ? " 7\n" : " 0\n");
    write(bytes);
    ProjectControllerTestAccess::newProject(controller);
    ASSERT_EQ(OperationTestAccess::start(controller, [&](const auto &snapshot, const WorkerControl &control) {
        ObservedFile file(source, counters);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
            throw std::runtime_error("test source open failed");
        auto writer = cache::CacheWriter::create(area.filePath("observed.cache"), limits, area.path());
        if (!writer) throw std::runtime_error("test writer creation failed");
        auto result = io::parseAscXyz(file, [&](auto batch) {
            auto status = writer.value()->append(batch);
            if (status) counters.measurements += batch.size();
            return status;
        }, [&](auto z) { return control.confirmNonZeroZ(z); },
            [](void *p) noexcept { return static_cast<const WorkerControl *>(p)->cancelled(); },
            const_cast<WorkerControl *>(&control));
        if (!result) return WorkerOutcome::cancelled();
        EXPECT_EQ(result.value().measurements, 12000);
        EXPECT_EQ(result.value().bytesRead, bytes.size());
        auto finished = writer.value()->finish();
        if (!finished) throw std::runtime_error("test writer finish failed");
        auto reader = cache::CacheReader::open(finished.value().path(), limits);
        if (!reader) throw std::runtime_error("test reader open failed");
        EXPECT_EQ(reader.value()->metadata().pointCount, 12000);
        EXPECT_TRUE(reader.value()->validate(limits, area.path()));
        return success(snapshot);
    }).outcome, CommandOutcome::Applied);
    const auto z = pending();
    EXPECT_EQ(z.line, 6000);
    const auto reads = counters.reads.load();
    EXPECT_GT(reads, 0);
    EXPECT_EQ(counters.measurements.load(), 4096);
    for (int i = 0; i != 10; ++i) OperationTestAccess::poll(controller);
    EXPECT_EQ(counters.reads.load(), reads);
    EXPECT_EQ(counters.measurements.load(), 4096);
    EXPECT_EQ(counters.seeks.load(), 0);
    controller.answerNonZeroZ(z, io::ZDecision::Continue);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(counters.measurements.load(), 12000);
    EXPECT_EQ(counters.seeks.load(), 0);
}

TEST_F(ZConfirmation, ContinueAndCommitWorkWithoutProcessingQtEvents)
{
    write("1 -1 4\n2 -2 0\n");
    start();
    controller.answerNonZeroZ(pending(), io::ZDecision::Continue);
    const auto c = context();
    ASSERT_TRUE(waitContext(c, false));
    OperationTestAccess::ready(controller, controller.operation().id);
    ASSERT_TRUE(c->nativeCompletion.wait());
    OperationTestAccess::poll(controller);
    EXPECT_FALSE(controller.operation().active);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    readCache(2);
}

TEST_F(ZConfirmation, ShutdownAfterPublicationBeforeWaitCannotLoseWakeup)
{
    Event published, release;
    OperationRunnerTestAccess::zHooks(OperationTestAccess::runner(controller), {}, [&] { published.set(); release.wait(); });
    start();
    published.wait();
    const auto z = *controller.pendingNonZeroZ();
    controller.requestShutdown();
    EXPECT_EQ(controller.answerNonZeroZ(z, io::ZDecision::Continue).outcome, CommandOutcome::Rejected);
    release.set();
    cancelled();
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
}

TEST_F(ZConfirmation, PollDuringStartDefersNotificationUntilImportCommandReturns)
{
    bool polled = false, returned = false;
    int requests = 0;
    QObject::connect(&controller, &ProjectController::operationChanged, [&] {
        if (!polled && controller.operation().active) {
            polled = true;
            EXPECT_TRUE(waiting());
            OperationTestAccess::poll(controller); // nested event delivery under start command guard
        }
    });
    QObject::connect(&controller, &ProjectController::nonZeroZConfirmationChanged, [&] {
        if (auto z = controller.pendingNonZeroZ()) {
            EXPECT_TRUE(returned);
            ++requests;
            EXPECT_EQ(controller.answerNonZeroZ(*z, io::ZDecision::Continue).outcome, CommandOutcome::Applied);
        }
    });
    start();
    returned = true;
    EXPECT_EQ(requests, 0);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(requests, 1);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}
