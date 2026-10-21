#include "operation_test_support.h"
#include "project_test_access.h"
#include "cache_file_test_access.h"
#include "cache/cache_reader.h"
#include "persistence/project_manifest.h"

#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <thread>

using namespace cadcontour;
using namespace cadcontour::test;

namespace {
class ImportApplication : public testing::Test {
protected:
    QTemporaryDir area;
    QString root = area.path() + QStringLiteral("/storage");
    QString source = area.path() + QStringLiteral("/облако.xyz");
    ProjectController controller{root};
    cache::ResourceLimits limits{8 * 1024 * 1024, 64 * 1024 * 1024, 8};

    void SetUp() override { ASSERT_TRUE(area.isValid()); write("-0 2 0\n3 -4 -0\n3 -4 0\n"); }
    void write(const QByteArray &bytes)
    {
        QFile file(source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }
    ImportRequest request(bool confirmed = false) const { return {source, limits, confirmed}; }
    QString cachePath() const
    { return QString::fromStdWString(controller.project()->dataset()->cachePath().wstring()); }
    int attempts() const { return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(); }
    void initial()
    {
        ASSERT_EQ(controller.importAscXyz(request()).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        ASSERT_NE(controller.project(), nullptr);
    }
    void awaitReady()
    {
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_TRUE(context);
        ASSERT_TRUE(waitContext(context, false));
    }
    QString pendingPath()
    {
        const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        std::lock_guard lock(context->mutex);
        const auto &info = *context->outcome.imported;
        return QDir(info.storageDirectory).filePath(info.relativeCachePath);
    }
    void readCache(const QString &path, std::uint64_t count = 3)
    {
        auto opened = cache::CacheReader::open(path, limits);
        ASSERT_TRUE(opened);
        EXPECT_EQ(opened.value()->metadata().pointCount, count);
        auto read = opened.value()->scanAll([](auto) { return cache::success(); });
        ASSERT_TRUE(read);
        EXPECT_EQ(read.value().returnedRecords, count);
    }
};
} // namespace

TEST_F(ImportApplication, FirstImportMetadataHistoryAndCacheIndependentOfSource)
{
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(controller.newProject().failure->detail, ErrorDetail::ImportRequired);
    initial();
    const auto &dataset = *controller.project()->dataset();
    EXPECT_GT(controller.project()->runtimeIdentity(), 0);
    EXPECT_EQ(controller.project()->inputRevision(), 0);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    EXPECT_EQ(dataset.pointCount, 3);
    EXPECT_DOUBLE_EQ(dataset.minimum.x, -0.0);
    EXPECT_TRUE(std::signbit(dataset.minimum.x));
    EXPECT_DOUBLE_EQ(dataset.minimum.y, -4);
    EXPECT_DOUBLE_EQ(dataset.maximum.x, 3);
    EXPECT_DOUBLE_EQ(dataset.maximum.y, 2);
    EXPECT_FALSE(dataset.hasNonZeroZ);
    EXPECT_EQ(dataset.sourceLines, 3);
    EXPECT_EQ(dataset.sourceBytes, 22);
    EXPECT_EQ(QString::fromStdWString(dataset.sourcePath.wstring()), source);
    EXPECT_FALSE(dataset.location->relativeCachePath.is_absolute());
    auto reader = cache::CacheReader::open(cachePath(), limits);
    ASSERT_TRUE(reader);
    EXPECT_EQ(reader.value()->metadata().id, dataset.cacheId);
    ASSERT_TRUE(QFile::remove(source));
    readCache(cachePath());
    auto checked = reader.value()->validate(limits, area.path());
    ASSERT_TRUE(checked);
    EXPECT_EQ(attempts(), 1);
    QDirIterator dirs(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (dirs.hasNext())
        EXPECT_FALSE(QFileInfo(dirs.next()).fileName().startsWith(QStringLiteral("scratch-")));
}

TEST_F(ImportApplication, PersistentIdentitySurvivesCellUndoRedoAndFailedReplacement)
{
    initial();
    const auto id = controller.project()->projectId();
    EXPECT_FALSE(id.isNull());
    controller.setCell(0.125);
    controller.undo();
    controller.redo();
    EXPECT_EQ(controller.project()->projectId(), id);
    const auto snapshot = persistence::captureSnapshot(*controller.project(), QUuid::createUuid(), QUuid::createUuid());
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().projectId, id);
    EXPECT_DOUBLE_EQ(snapshot.value().cell, 0.125);
    const auto bytes = persistence::encodeManifest(snapshot.value(), {65536, 4096, 32});
    ASSERT_TRUE(bytes);
    EXPECT_FALSE(bytes.value().contains("runtimeIdentity"));
    EXPECT_FALSE(bytes.value().contains("Revision"));
    write("bad input\n");
    controller.importAscXyz(request(true));
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.project()->projectId(), id);
    EXPECT_TRUE(controller.canUndo());
}

TEST_F(ImportApplication, PersistentIdentityIsIndependentAcrossRuntimeInstancesAndReplacements)
{
    initial();
    const auto first = controller.project()->projectId();
    ProjectController other(root);
    ASSERT_EQ(other.importAscXyz(request()).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(other));
    ASSERT_NE(other.project(), nullptr);
    EXPECT_EQ(other.project()->runtimeIdentity(), controller.project()->runtimeIdentity());
    EXPECT_NE(other.project()->projectId(), first);
    ASSERT_TRUE(finishBackgroundAutosave(controller));
    ASSERT_EQ(controller.importAscXyz(request(true)).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_NE(controller.project()->projectId(), first);
}

TEST_F(ImportApplication, SharedPhysicalBindingRetainsOldFileAcrossReplacementAndClose)
{
    initial();
    auto binding = controller.project()->dataset()->location;
    const auto oldPath = QString::fromStdWString(binding->cachePath().wstring());
    controller.importAscXyz(request(true));
    ASSERT_TRUE(finish(controller));
    EXPECT_NE(controller.project()->dataset()->location, binding);
    EXPECT_EQ(QString::fromStdWString(binding->cachePath().wstring()), oldPath);
    controller.closeProject();
    binding.reset();
    readCache(oldPath);
}

TEST_F(ImportApplication, ConfirmedReplacementResetsHistoryAndRetainsBothCaches)
{
    initial();
    const auto oldPath = cachePath();
    const auto oldId = controller.project()->runtimeIdentity();
    const auto oldCacheId = controller.project()->dataset()->cacheId;
    controller.setCell(0.5);
    controller.setCell(0.25);
    controller.undo();
    EXPECT_TRUE(controller.canUndo());
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(controller.importAscXyz(request()).failure->detail, ErrorDetail::ReplacementNotConfirmed);
    EXPECT_FALSE(controller.operation().active);
    ASSERT_EQ(controller.importAscXyz(request(true)).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project()->runtimeIdentity(), oldId);
    EXPECT_TRUE(controller.canRedo());
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_GT(controller.project()->runtimeIdentity(), oldId);
    EXPECT_NE(controller.project()->dataset()->cacheId, oldCacheId);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
    EXPECT_EQ(controller.project()->inputRevision(), 0);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::NoChange);
    readCache(oldPath);
    readCache(cachePath());
    EXPECT_EQ(attempts(), 2);
}

TEST_F(ImportApplication, FailuresPreserveStructuredRootCauseAndCleanAttempt)
{
    struct Case { QByteArray text; cache::ErrorCode code; };
    for (const auto &item : {Case{"1 bad 0\n", cache::ErrorCode::ParseError},
                            Case{"1e999 2 0\n", cache::ErrorCode::NumericRange},
                            Case{"\n", cache::ErrorCode::InsufficientData},
                            Case{"1 2 3\n", cache::ErrorCode::InvalidInput}}) {
        write(item.text);
        ASSERT_EQ(controller.importAscXyz(request()).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.project(), nullptr);
        ASSERT_TRUE(controller.operation().failure);
        const auto &failure = *controller.operation().failure;
        EXPECT_EQ(failure.reason, FailureReason::ImportFailed);
        ASSERT_TRUE(failure.importError);
        EXPECT_EQ(failure.importError->code, item.code);
        EXPECT_EQ(attempts(), 0);
    }
    QFile::remove(source);
    controller.importAscXyz(request());
    ASSERT_TRUE(finish(controller));
    ASSERT_TRUE(controller.operation().failure->importError);
    EXPECT_EQ(controller.operation().failure->importError->code, cache::ErrorCode::OpenFailed);
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ImportApplication, FailedReplacementPreservesProjectAndRedo)
{
    initial();
    const auto stamp = controller.project()->operationStamp();
    const auto path = cachePath();
    controller.setCell(2);
    controller.undo();
    write("1 wrong 0\n");
    controller.importAscXyz(request(true));
    ASSERT_TRUE(finish(controller));
    EXPECT_TRUE(controller.project()->accepts({stamp.projectIdentity, 2}));
    EXPECT_EQ(cachePath(), path);
    EXPECT_TRUE(controller.canRedo());
    const auto &error = *controller.operation().failure->importError;
    EXPECT_EQ(error.code, cache::ErrorCode::ParseError);
    ASSERT_TRUE(error.source);
    EXPECT_EQ(error.source->line, 1);
    EXPECT_EQ(error.source->coordinate, cache::Coordinate::Y);
    EXPECT_EQ(attempts(), 1);
}

TEST_F(ImportApplication, ResourceAndStorageFailuresDoNotCreateProject)
{
    for (int mode = 0; mode != 2; ++mode) {
        auto input = request();
        if (mode == 0) input.limits.memoryBytes = 1;
        else input.limits.temporaryDiskBytes = 1;
        controller.importAscXyz(input);
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().failure->importError->code, cache::ErrorCode::ResourceExhausted);
        EXPECT_EQ(controller.project(), nullptr);
        EXPECT_EQ(attempts(), 0);
    }
    ProjectController blocked(source); // root is an existing file
    blocked.importAscXyz(request());
    ASSERT_TRUE(finish(blocked));
    EXPECT_EQ(blocked.operation().failure->importError->code, cache::ErrorCode::OpenFailed);
    EXPECT_EQ(blocked.project(), nullptr);
}

TEST_F(ImportApplication, ControlledZContinueAndCancelRemainHeadless)
{
    write("1 2 0\n3 4 5\n6 7 9\n");
    int calls = 0;
    OperationTestAccess::import(controller, request(), [&](io::NonZeroZ z) {
        EXPECT_NE(QThread::currentThread(), controller.thread());
        EXPECT_EQ(z.line, 2);
        ++calls;
        return io::ZDecision::Cancel;
    });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_EQ(attempts(), 0);
    OperationTestAccess::import(controller, request(), [&](io::NonZeroZ) {
        ++calls; return io::ZDecision::Continue;
    });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(calls, 2);
    ASSERT_NE(controller.project(), nullptr);
    EXPECT_TRUE(controller.project()->dataset()->hasNonZeroZ);
    readCache(cachePath());
}

TEST_F(ImportApplication, CancellationBeforeEntryAndAtReadyPreservesOriginalContext)
{
    for (bool replace : {false, true}) {
        if (replace) { initial(); controller.setCell(2); controller.undo(); }
        const auto *original = controller.project();
        for (bool beforeEntry : {true, false}) {
            Event entered, release;
            if (beforeEntry)
                OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
                    [&](std::function<void()> entry) {
                        return std::unique_ptr<QThread>(QThread::create([&, entry = std::move(entry)] {
                            entered.set(); release.wait(); entry();
                        }));
                    });
            controller.importAscXyz(request(replace));
            if (beforeEntry) entered.wait(); else awaitReady();
            EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
            release.set();
            ASSERT_TRUE(finish(controller));
            EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
            EXPECT_EQ(controller.project(), original);
            EXPECT_EQ(attempts(), replace ? 1 : 0);
            if (replace) EXPECT_TRUE(controller.canRedo());
        }
    }
}

TEST_F(ImportApplication, CancellationDuringRealParsingCleansAttempt)
{
    write("1 2 3\n4 5 0\n");
    Event inParser, resume;
    OperationTestAccess::import(controller, request(), [&](io::NonZeroZ) {
        inParser.set(); resume.wait(); return io::ZDecision::Continue;
    });
    inParser.wait();
    controller.cancelOperation();
    resume.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ImportApplication, PreparationFailureCancellationAndInvalidationDoNotPublish)
{
    initial();
    const auto *previous = controller.project();
    controller.setCell(2);
    controller.undo();
    for (int mode = 0; mode < 6; ++mode) {
        OperationTestAccess::preparation(controller, [&, mode] {
            EXPECT_EQ(controller.project(), previous);
            EXPECT_TRUE(controller.canRedo());
            if (mode == 0) throw std::bad_alloc();
            if (mode == 1) throw std::runtime_error("candidate refused");
            if (mode == 2) controller.cancelOperation();
            if (mode == 3) OperationTestAccess::invalidate(controller);
            if (mode == 4) OperationTestAccess::runner(controller).cancel();
            if (mode == 5) OperationTestAccess::runner(controller).shutdown();
        });
        controller.importAscXyz(request(true));
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_EQ(controller.project(), previous);
        EXPECT_TRUE(controller.canRedo());
        EXPECT_EQ(attempts(), 1);
        if (mode == 0) EXPECT_EQ(controller.operation().failure->reason, FailureReason::OutOfMemory);
        if (mode == 1) EXPECT_EQ(controller.operation().failure->reason, FailureReason::CandidatePreparationFailed);
        const auto id = controller.operation().id;
        OperationTestAccess::ready(controller, id);
        EXPECT_EQ(controller.project(), previous);
    }
}

TEST_F(ImportApplication, InvalidMetadataMissingCacheAndWrongProductAreDiscarded)
{
    for (int mode = 0; mode != 5; ++mode) {
        OperationTestAccess::importWork(controller, request(), [mode](WorkerOutcome &outcome) {
            ASSERT_TRUE(outcome.imported);
            auto &ready = outcome.imported->ready;
            if (mode == 0) ready.metadata.pointCount = 0;
            if (mode == 1) ready.source.measurements++;
            if (mode == 2) ready.metadata.id = {};
            if (mode == 3) QFile::remove(outcome.imported->data.cache.path());
            if (mode == 4) {
                outcome.imported.reset();
                outcome.product = std::make_unique<VerificationProduct>();
            }
        });
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_EQ(controller.operation().failure->reason, FailureReason::InvalidProduct);
        EXPECT_EQ(controller.project(), nullptr);
        EXPECT_EQ(attempts(), 0);
    }
}

TEST_F(ImportApplication, NoProjectAbaAndReplacementCloseRevokeOldResult)
{
    for (int mode = 0; mode != 4; ++mode) {
        if (mode > 0 && !controller.project()) initial();
        controller.importAscXyz(request(controller.project() != nullptr));
        awaitReady();
        const auto path = pendingPath();
        const auto id = controller.operation().id;
        if (mode == 0) { // NoProject -> different context -> NoProject
            ProjectControllerTestAccess::newProject(controller);
            controller.closeProject();
        } else if (mode == 1) controller.closeProject();
        else if (mode == 2) ProjectControllerTestAccess::newProject(controller);
        else { controller.setCell(2); controller.undo(); }
        OperationTestAccess::ready(controller, id);
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_FALSE(QFileInfo::exists(path));
    }
}

TEST_F(ImportApplication, RejectedAndNoOpCommandsKeepImportApplicable)
{
    initial();
    controller.importAscXyz(request(true));
    awaitReady();
    EXPECT_EQ(controller.setCell(1).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.setCell(-1).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.newProject().failure->detail, ErrorDetail::ImportRequired);
    EXPECT_EQ(controller.importAscXyz(request(true)).failure->category, CommandError::Busy);
    EXPECT_FALSE(controller.operation().publicationRevoked);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}

TEST_F(ImportApplication, ObserversSeeConsistentSessionAndAcceptAndRejectReentry)
{
    initial();
    controller.setCell(2);
    controller.undo();
    int notifications = 0;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        ++notifications;
        ASSERT_TRUE(controller.project()->dataset());
        EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
        EXPECT_FALSE(controller.canUndo());
        EXPECT_FALSE(controller.canRedo());
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
        for (const auto &result : {controller.setCell(2), controller.undo(), controller.redo(),
            controller.closeProject(), controller.importAscXyz(request(true))})
            EXPECT_EQ(result.failure->detail, ErrorDetail::ReentrantCommand);
    });
    controller.importAscXyz(request(true));
    awaitReady();
    const auto id = controller.operation().id;
    OperationTestAccess::ready(controller, id);
    OperationTestAccess::ready(controller, id);
    EXPECT_EQ(notifications, 1);
    EXPECT_FALSE(OperationTestAccess::runner(controller).decide(id, PublicationDecision::Discard));
    EXPECT_FALSE(OperationTestAccess::runner(controller).decide(id, PublicationDecision::Accept));
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(notifications, 1);
}

TEST_F(ImportApplication, GuardDestructionRunsOnWorkerForAcceptDiscardAndExecutionException)
{
    for (int mode = 0; mode != 3; ++mode) {
        std::atomic<bool> destroyed = false;
        QString path;
        OperationTestAccess::importWork(controller, request(controller.project() != nullptr), [&](WorkerOutcome &outcome) {
            ASSERT_TRUE(outcome.imported);
            auto *guard = cache::CacheFileTestAccess::guard(outcome.imported->data.cache);
            const auto worker = QThread::currentThread();
            EXPECT_EQ(guard->thread(), worker);
            EXPECT_TRUE(guard->autoRemove());
            path = outcome.imported->data.cache.path();
            QObject::connect(guard, &QObject::destroyed, [&, worker] {
                EXPECT_EQ(QThread::currentThread(), worker);
                destroyed.store(true);
            });
            if (mode == 2) throw std::runtime_error("failure with a completed PendingCache");
        });
        awaitReady();
        if (mode == 1) controller.cancelOperation();
        ASSERT_TRUE(finish(controller));
        EXPECT_TRUE(destroyed.load());
        EXPECT_EQ(QFileInfo::exists(path), mode == 0);
    }
}

TEST_F(ImportApplication, AcceptThenCloseAndShutdownBeforeWorkerFinalizationRetainsCache)
{
    Event finalizing, release;
    OperationRunnerTestAccess::beforeOwnershipFinalization(OperationTestAccess::runner(controller),
        [&](PublicationDecision decision) {
            EXPECT_EQ(decision, PublicationDecision::Accept);
            finalizing.set(); release.wait();
        });
    controller.importAscXyz(request());
    awaitReady();
    const auto path = pendingPath();
    OperationTestAccess::ready(controller, controller.operation().id);
    finalizing.wait();
    ASSERT_NE(controller.project(), nullptr);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(controller.importAscXyz(request()).failure->category, CommandError::Busy);
    controller.requestShutdown();
    OperationTestAccess::poll(controller);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    EXPECT_TRUE(OperationTestAccess::runner(controller).busy());
    release.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
    readCache(path);
}

TEST_F(ImportApplication, ShutdownDuringBorrowedPreparationDiscardsAfterLeaseRelease)
{
    QString path;
    OperationTestAccess::preparation(controller, [&] {
        path = pendingPath();
        controller.requestShutdown();
        const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        {
            std::lock_guard lock(context->mutex);
            EXPECT_TRUE(context->leased);
            EXPECT_EQ(context->decision, PublicationDecision::Discard);
        }
        EXPECT_TRUE(QFileInfo::exists(path));
        EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    });
    controller.importAscXyz(request());
    ASSERT_TRUE(finish(controller));
    EXPECT_FALSE(QFileInfo::exists(path));
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(attempts(), 0);
}

TEST_F(ImportApplication, DestructorDrainsReadyAndAcceptedWorkerWithoutGuiCallbacks)
{
    for (bool accept : {false, true}) {
        Event finalizing, allow;
        auto owner = std::make_unique<ProjectController>(root);
        OperationRunnerTestAccess::beforeOwnershipFinalization(OperationTestAccess::runner(*owner),
            [&](PublicationDecision decision) {
                EXPECT_EQ(decision, accept ? PublicationDecision::Accept : PublicationDecision::Discard);
                finalizing.set(); allow.wait();
            });
        owner->importAscXyz(request());
        const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(*owner));
        ASSERT_TRUE(waitContext(context, false));
        QString path;
        {
            std::lock_guard lock(context->mutex);
            const auto &info = *context->outcome.imported;
            path = QDir(info.storageDirectory).filePath(info.relativeCachePath);
        }
        if (accept) OperationTestAccess::ready(*owner, owner->operation().id);
        std::thread helper([&] { finalizing.wait(); EXPECT_TRUE(QFileInfo::exists(path)); allow.set(); });
        owner.reset();
        helper.join();
        EXPECT_EQ(QFileInfo::exists(path), accept);
        if (accept) readCache(path);
        EXPECT_FALSE(context->outcome.product);
    }
}

TEST_F(ImportApplication, VerificationStillWorksAfterRealImport)
{
    initial();
    OperationTestAccess::start(controller, [](const auto &snapshot, const auto &) { return success(snapshot, 73); });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(OperationTestAccess::value(controller), 73);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(controller.project()->dataset()->pointCount, 3);
}

TEST_F(ImportApplication, ImportRetainsBusyAndShutdownUntilNativeTlsCompletion)
{
    Event nativeCleanup, allow;
    struct ExitBarrier {
        Event &entered;
        Event &allow;
        ~ExitBarrier() { entered.set(); allow.wait(); }
    };
    OperationTestAccess::importWork(controller, request(), [&](WorkerOutcome &) {
        thread_local std::unique_ptr<ExitBarrier> barrier;
        barrier = std::make_unique<ExitBarrier>(nativeCleanup, allow);
    });
    awaitReady();
    OperationTestAccess::ready(controller, controller.operation().id);
    const auto path = cachePath();
    nativeCleanup.wait();
    const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    EXPECT_FALSE(context->nativeCompletion.finished());
    OperationTestAccess::poll(controller);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.importAscXyz(request(true)).failure->category, CommandError::Busy);
    controller.requestShutdown();
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    allow.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    readCache(path);
}

TEST_F(ImportApplication, AcceptedNotificationCanRequestShutdownWithoutRevokingAccept)
{
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
        controller.requestShutdown();
    });
    controller.importAscXyz(request());
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    readCache(cachePath());
}

TEST_F(ImportApplication, ImportIdentityExhaustionLeavesExistingSessionAndHistory)
{
    initial();
    controller.setCell(2);
    controller.undo();
    const auto *previous = controller.project();
    ProjectControllerTestAccess::setLastIdentity(controller, std::numeric_limits<std::uint64_t>::max());
    controller.importAscXyz(request(true));
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().failure->reason, FailureReason::CandidatePreparationFailed);
    EXPECT_EQ(controller.project(), previous);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(attempts(), 1);
}

TEST_F(ImportApplication, InvalidRequestWrongThreadAndExhaustedOperationIdDoNotStart)
{
    auto empty = request();
    empty.sourcePath.clear();
    EXPECT_EQ(controller.importAscXyz(empty).failure->category, CommandError::InvalidValue);
    std::thread wrong([&] {
        EXPECT_EQ(controller.importAscXyz(request()).failure->detail, ErrorDetail::WrongThread);
    });
    wrong.join();
    OperationTestAccess::setLastId(controller, std::numeric_limits<OperationId>::max());
    EXPECT_EQ(controller.importAscXyz(request()).failure->detail, ErrorDetail::OperationIdExhausted);
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_FALSE(controller.operation().active);
    EXPECT_EQ(attempts(), 0);
}
