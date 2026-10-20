#include "operation_test_support.h"
#include "save_test_access.h"
#include "cache/cache_reader.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <vector>

#define NOMINMAX
#include <Windows.h>

using namespace cadcontour;
using namespace cadcontour::test;
using namespace cadcontour::persistence;

namespace {
class Save : public testing::Test {
protected:
    QTemporaryDir area;
    QString source = area.path() + QStringLiteral("/исходные точки.xyz");
    QString target = area.path() + QStringLiteral("/Проект A/Деталь.pc2dproj");
    ProjectController controller{area.path() + QStringLiteral("/import")};
    cache::ResourceLimits limits{8ULL << 20, 64ULL << 20, 8};
    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("-0 2 0\n3 -4 0\n"), 14);
        file.close();
        ASSERT_EQ(controller.importAscXyz({source, limits}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    }
    QByteArray bytes(const QString &path)
    { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
    ProjectSnapshot manifest(const QString &path)
    {
        QFile f(path); EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        auto r = readManifest(f, {4ULL << 20, 4096, 32}); EXPECT_TRUE(r);
        return r ? r.value() : ProjectSnapshot{};
    }
    QString datasetPath(const ProjectSnapshot &s, const QString &path)
    { return QDir(QFileInfo(path).absolutePath()).filePath(s.datasets.at(0).relativePath); }
    QString cachePath() const { return QString::fromStdWString(controller.project()->dataset()->cachePath().wstring()); }
    void saved()
    {
        ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    }
    void noStaging()
    {
        QDirIterator it(area.path(), {QStringLiteral(".save-*")}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        EXPECT_FALSE(it.hasNext());
    }
    void ready()
    {
        auto c = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_TRUE(c); ASSERT_TRUE(waitContext(c, true));
    }
};
}

TEST_F(Save, FirstSavePortableBytesBindingAndSourceIndependence)
{
    EXPECT_TRUE(controller.isDirty());
    const auto old = controller.project()->dataset()->location;
    const auto id = controller.project()->projectId();
    const auto original = bytes(cachePath());
    ASSERT_TRUE(QFile::remove(source));
    saved();
    const auto s = manifest(target);
    EXPECT_EQ(s.projectId, id);
    EXPECT_EQ(s.pointCacheId, controller.project()->dataset()->cacheId);
    EXPECT_NE(controller.project()->dataset()->location, old);
    EXPECT_EQ(bytes(datasetPath(s, target)), original);
    EXPECT_TRUE(QFileInfo::exists(QString::fromStdWString(old->cachePath().wstring())));
    EXPECT_EQ(QFileInfo(cachePath()).canonicalFilePath(), QFileInfo(datasetPath(s, target)).canonicalFilePath());
    EXPECT_FALSE(controller.isDirty());
    auto r = cache::CacheReader::open(cachePath(), limits); ASSERT_TRUE(r);
    EXPECT_TRUE(r.value()->validate(limits, area.path()));
    noStaging();
}

TEST_F(Save, RepeatedMetadataSaveDoesNotCopyOrScanCache)
{
    saved(); const auto before = manifest(target);
    const auto identity = controller.savedProject()->datasetIdentity;
    const auto cache = bytes(cachePath());
    SaveTestAccess::configure(controller, [](SavePhase p) {
        EXPECT_NE(p, SavePhase::CopyRead); EXPECT_NE(p, SavePhase::CopyWrite); EXPECT_NE(p, SavePhase::CopyVerify);
    });
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    const auto after = manifest(target);
    EXPECT_NE(after.snapshotId, before.snapshotId); EXPECT_EQ(after.storageId, before.storageId);
    EXPECT_EQ(controller.savedProject()->datasetIdentity, identity);
    EXPECT_DOUBLE_EQ(after.cell, 0.5); EXPECT_EQ(bytes(cachePath()), cache);
    EXPECT_FALSE(controller.isDirty()); EXPECT_TRUE(controller.canUndo());
    noStaging();
}

TEST_F(Save, SaveAsKeepsLogicalIdentityAndOldHolders)
{
    saved(); const auto before = manifest(target); const auto oldBytes = bytes(target);
    const auto old = controller.project()->dataset()->location;
    const auto original = bytes(cachePath());
    const auto other = area.path() + QStringLiteral("/Проект B/Новая деталь.pc2dproj");
    ASSERT_EQ(controller.saveProjectAs({other}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    const auto after = manifest(other);
    EXPECT_EQ(after.projectId, before.projectId); EXPECT_EQ(after.pointCacheId, before.pointCacheId);
    EXPECT_NE(after.storageId, before.storageId); EXPECT_NE(after.snapshotId, before.snapshotId);
    EXPECT_EQ(bytes(target), oldBytes); EXPECT_EQ(bytes(datasetPath(after, other)), original);
    EXPECT_EQ(bytes(QString::fromStdWString(old->cachePath().wstring())), original);
    EXPECT_EQ(controller.savedProject()->manifestPath, other); EXPECT_FALSE(controller.isDirty());
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_EQ(bytes(QString::fromStdWString(old->cachePath().wstring())), original);
}

TEST_F(Save, EquivalentSaveAsReusesStorageAndSameDirectoryRenameIsExplicitConflict)
{
    saved(); const auto before = manifest(target);
    SaveTestAccess::configure(controller, [](SavePhase p) { EXPECT_NE(p, SavePhase::CopyRead); });
    const auto equivalentPath = QFileInfo(target).absolutePath() + QStringLiteral("/../Проект A/Деталь.pc2dproj");
    ASSERT_EQ(controller.saveProjectAs({equivalentPath}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(manifest(target).storageId, before.storageId);
    const auto prior = bytes(target);
    ASSERT_EQ(controller.saveProjectAs({QFileInfo(target).absolutePath() + QStringLiteral("/Other.pc2dproj")}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller)); EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::Conflict);
    EXPECT_EQ(bytes(target), prior);
}

TEST_F(Save, SnapshotDirtyUndoRedoAndHistorySurviveSave)
{
    Event blocked, proceed;
    SaveTestAccess::configure(controller, [&](SavePhase p) {
        if (p == SavePhase::Source) { blocked.set(); proceed.wait(); }
    });
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); blocked.wait();
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::NoChange);
    EXPECT_FALSE(controller.operation().publicationRevoked);
    proceed.set(); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_DOUBLE_EQ(manifest(target).cell, 1); EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_TRUE(controller.isDirty()); EXPECT_TRUE(controller.canUndo());
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_FALSE(controller.isDirty());
    EXPECT_EQ(controller.redo().outcome, CommandOutcome::Applied); EXPECT_TRUE(controller.isDirty());
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_TRUE(controller.canRedo()); EXPECT_FALSE(controller.isDirty());
}

TEST_F(Save, AdmissionBusyAndNoProject)
{
    ProjectController empty(area.path() + QStringLiteral("/empty"));
    EXPECT_EQ(empty.saveProject({target}).failure->category, CommandError::NoProject);
    EXPECT_FALSE(empty.isDirty());
    EXPECT_EQ(controller.saveProject().outcome, CommandOutcome::Rejected);
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ready();
    EXPECT_EQ(controller.saveProject({target}).failure->category, CommandError::Busy);
    EXPECT_EQ(controller.importAscXyz({source, limits, true}).failure->category, CommandError::Busy);
    ASSERT_TRUE(finish(controller));
}

TEST_F(Save, CancelAtReadyDiscardsWithoutPublishingManifest)
{
    const auto old = controller.project()->dataset()->location;
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ready();
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller)); EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_FALSE(QFileInfo::exists(target)); EXPECT_EQ(controller.project()->dataset()->location, old);
    EXPECT_EQ(controller.savedProject(), nullptr); EXPECT_TRUE(controller.isDirty()); noStaging();
}

TEST_F(Save, CloseBeforePermissionCannotPublishIntoReplacementSession)
{
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ready();
    const auto id = controller.operation().id;
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    OperationTestAccess::ready(controller, id);
    ASSERT_TRUE(finish(controller)); EXPECT_FALSE(QFileInfo::exists(target)); EXPECT_EQ(controller.project(), nullptr);
    ASSERT_EQ(controller.importAscXyz({source, limits}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    OperationTestAccess::ready(controller, id);
    EXPECT_TRUE(controller.isDirty()); EXPECT_EQ(controller.savedProject(), nullptr);
}

TEST_F(Save, PermissionIsNotSuccessAndShutdownWaitsForCommit)
{
    Event permission, proceed;
    SaveTestAccess::configure(controller, [&](SavePhase p) {
        if (p == SavePhase::BeforeCommit) { permission.set(); proceed.wait(); }
    });
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ready();
    OperationTestAccess::ready(controller, controller.operation().id); permission.wait();
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
    EXPECT_EQ(controller.operation().status, OperationStatus::Finishing);
    EXPECT_EQ(controller.savedProject(), nullptr); EXPECT_FALSE(QFileInfo::exists(target));
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.closeProject().failure->category, CommandError::Busy);
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    proceed.set(); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready); EXPECT_FALSE(controller.isDirty());
    EXPECT_EQ(manifest(target).projectId, controller.project()->projectId());
}

TEST_F(Save, ShutdownDuringCopyCancelsAndJoins)
{
    Event entered, proceed;
    SaveTestAccess::configure(controller, [&](SavePhase p) {
        if (p == SavePhase::CopyRead) { entered.set(); proceed.wait(); }
    });
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); entered.wait();
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied); proceed.set();
    ASSERT_TRUE(finish(controller)); EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled); EXPECT_FALSE(QFileInfo::exists(target)); noStaging();
}

TEST_F(Save, ShutdownFromPersistedStateNotificationWaitsForTerminalCompletion)
{
    auto &runner = OperationTestAccess::runner(controller);
    int publications = 0, terminals = 0, releases = 0;
    std::vector<ShutdownState> shutdownStates;
    auto checkReady = [&] {
        if (controller.shutdownState() == ShutdownState::Ready) {
            EXPECT_FALSE(controller.operation().active);
            EXPECT_FALSE(runner.busy());
        }
    };
    QObject::connect(&controller, &ProjectController::operationChanged, &controller, [&] {
        checkReady();
        if (!controller.operation().active) {
            ++terminals;
            EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
            EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
        }
    });
    QObject::connect(&controller, &ProjectController::shutdownChanged, &controller, [&] {
        checkReady();
        shutdownStates.push_back(controller.shutdownState());
        EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::NoChange);
    });
    QObject::connect(&runner, &OperationRunner::released, &controller, [&](OperationId, WorkerStatus status,
        OperationFailure, bool) {
        ++releases;
        EXPECT_EQ(status, WorkerStatus::Succeeded);
        EXPECT_FALSE(controller.operation().active);
        checkReady();
    });
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied);
    const auto id = controller.operation().id;
    const auto context = OperationRunnerTestAccess::context(runner);
    ASSERT_TRUE(context);
    QObject::connect(&controller, &ProjectController::stateChanged, &controller, [&] {
        ++publications;
        EXPECT_TRUE(context->nativeCompletion.finished());
        EXPECT_FALSE(runner.busy());
        EXPECT_TRUE(controller.operation().active);
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
        EXPECT_EQ(controller.operation().status, OperationStatus::Finishing);
        EXPECT_NE(controller.savedProject(), nullptr);
        EXPECT_FALSE(controller.isDirty());
        EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
        OperationTestAccess::poll(controller); // Reentry must leave terminal completion to released().
        EXPECT_TRUE(controller.operation().active);
        checkReady();
    });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(shutdownStates, (std::vector<ShutdownState>{ShutdownState::Closing, ShutdownState::Ready}));
    EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Succeeded);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    ASSERT_NE(controller.savedProject(), nullptr);
    EXPECT_EQ(manifest(target).snapshotId, controller.savedProject()->snapshot.snapshotId);
    EXPECT_EQ(QFileInfo(cachePath()).canonicalFilePath(),
              QFileInfo(datasetPath(manifest(target), target)).canonicalFilePath());
    EXPECT_FALSE(controller.isDirty());
    emit runner.saveFinished(id, {}, {}); // A delayed receipt cannot publish or terminate again.
    for (int i = 0; i < 20; ++i) OperationTestAccess::poll(controller);
    EXPECT_EQ(publications, 1); EXPECT_EQ(terminals, 1); EXPECT_EQ(releases, 1);
    EXPECT_EQ(shutdownStates.size(), 2);
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::NoChange);
    noStaging();
}

TEST_F(Save, ReentrantAndDelayedNotificationsCannotStartOrApplyAnotherSave)
{
    saved(); const auto priorId = controller.operation().id;
    int notifications = 0;
    const auto connection = QObject::connect(&controller, &ProjectController::stateChanged, &controller, [&] {
        ++notifications;
        EXPECT_EQ(controller.saveProject().failure->detail, ErrorDetail::ReentrantCommand);
    });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    QObject::disconnect(connection); EXPECT_EQ(notifications, 1);
    const auto snapshotId = controller.savedProject()->snapshot.snapshotId;
    emit OperationTestAccess::runner(controller).saveFinished(priorId, {}, {});
    OperationTestAccess::ready(controller, priorId);
    EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, snapshotId);
}

TEST_F(Save, ManifestConflictAndCooperativeLockProtectExistingFiles)
{
    saved(); const auto before = bytes(target); const auto s = manifest(target);
    auto external = s; external.snapshotId = QUuid::createUuid();
    auto encoded = encodeManifest(external, {4ULL << 20, 4096, 32}); ASSERT_TRUE(encoded);
    QFile f(target); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); ASSERT_EQ(f.write(encoded.value()), encoded.value().size()); f.close();
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::Conflict);
    EXPECT_EQ(bytes(target), encoded.value()); EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, s.snapshotId);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)); ASSERT_EQ(f.write(before), before.size()); f.close();
    QLockFile lock(QFileInfo(target).absolutePath() + QStringLiteral("/.pc2d-save.lock")); ASSERT_TRUE(lock.tryLock());
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::Locked); EXPECT_EQ(bytes(target), before);
}

TEST_F(Save, ExistingForeignTargetAndDataAreNeverOverwritten)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(target).absolutePath()));
    QFile f(target); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); ASSERT_EQ(f.write("user data"), 9); f.close();
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::Conflict); EXPECT_EQ(bytes(target), "user data");
    ASSERT_TRUE(QFile::remove(target)); ASSERT_TRUE(QDir().mkpath(QFileInfo(target).absolutePath() + QStringLiteral("/data/foreign")));
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::Conflict); EXPECT_FALSE(QFileInfo::exists(target));
}

TEST_F(Save, FinalConflictCheckRunsAfterPermission)
{
    saved(); auto external = manifest(target); external.snapshotId = QUuid::createUuid();
    auto encoded = encodeManifest(external, {4ULL << 20, 4096, 32}); ASSERT_TRUE(encoded);
    SaveTestAccess::configure(controller, [&](SavePhase p) {
        if (p == SavePhase::BeforeCommit) {
            QFile f(target); if (!f.open(QIODevice::WriteOnly) || f.write(encoded.value()) != encoded.value().size())
                throw std::runtime_error("fixture write failed");
        }
    });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_EQ(bytes(target), encoded.value());
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::Conflict);
}

TEST_F(Save, UnknownManifestAppearingAfterFinalCheckIsNeverOverwritten)
{
    SaveTestAccess::configure(controller, [&](SavePhase p) {
        if (p == SavePhase::ManifestCommit) {
            QFile f(target);
            if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write("foreign document") != 16)
                throw std::runtime_error("fixture failed to create concurrent target");
        }
    });
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(bytes(target), "foreign document"); EXPECT_EQ(controller.savedProject(), nullptr);
    EXPECT_TRUE(controller.isDirty()); noStaging();
}

TEST_F(Save, MissingTruncatedOrModifiedImmutableCacheRejectsSave)
{
    saved(); const auto path = cachePath(); const auto original = bytes(path); const auto prior = bytes(target);
    ASSERT_TRUE(QFile::remove(path));
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::MissingDataset);
    QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write(original.left(8192)), 8192); file.close();
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write(original), original.size()); file.close();
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::ChangedDataset);
    EXPECT_EQ(bytes(target), prior);
}

TEST_F(Save, SourceEnvelopeAndPageCorruptionAreRejectedBeforeFirstSave)
{
    const auto path = cachePath(); const auto original = bytes(path);
    auto corrupt = original; corrupt[4096 + 100] ^= 1;
    QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); ASSERT_EQ(f.write(corrupt), corrupt.size()); f.close();
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_FALSE(QFileInfo::exists(target));
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::ChangedDataset); noStaging();
}

TEST_F(Save, DatasetCollisionAndDirectoryReplacementAreRejected)
{
    const auto cacheId = controller.project()->dataset()->cacheId;
    SaveTestAccess::configure(controller, [&](SavePhase p) {
        if (p == SavePhase::DatasetPublish) {
            QDirIterator dirs(QFileInfo(target).absolutePath(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            QString datasets;
            while (dirs.hasNext()) { const auto path = dirs.next(); if (QFileInfo(path).fileName() == u"datasets") datasets = path; }
            auto id = QUuid::fromRfc4122(QByteArrayView(reinterpret_cast<const char *>(cacheId.bytes.data()), 16));
            QFile f(datasets + u'/' + id.toString(QUuid::WithoutBraces) + QStringLiteral(".pc2dcache"));
            if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write("foreign") != 7) throw std::runtime_error("fixture");
            EXPECT_FALSE(QDir().rename(QFileInfo(target).absolutePath(), area.path() + QStringLiteral("/moved")));
        }
    });
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_FALSE(QFileInfo::exists(target)); noStaging();
}

TEST_F(Save, ShortDatasetAndManifestWritesAreFailures)
{
    for (bool manifestWrite : {false, true}) {
        const auto path = area.path() + (manifestWrite ? QStringLiteral("/short-manifest/P.pc2dproj") : QStringLiteral("/short-cache/P.pc2dproj"));
        bool writingManifest = false;
        SaveTestAccess::configure(controller, [&](SavePhase p) { writingManifest = p == SavePhase::ManifestWrite; },
            [manifestWrite, &writingManifest](QIODevice &device, const char *data, qint64 size) {
            if (writingManifest == manifestWrite) return device.write(data, size / 2);
            return device.write(data, size);
        });
        ASSERT_EQ(controller.saveProject({path}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_FALSE(QFileInfo::exists(path));
        EXPECT_EQ(controller.savedProject(), nullptr); noStaging();
    }
}

TEST_F(Save, UnconfirmedCommitRetainsFilesAndBlocksFurtherSave)
{
    saved(); const auto baseline = controller.savedProject()->snapshot.snapshotId;
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    SaveTestAccess::configure(controller, {}, {}, [](QSaveFile &file) { file.commit(); return false; });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::PublicationUncertain);
    EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, baseline); EXPECT_TRUE(controller.isDirty());
    ASSERT_NE(controller.unconfirmedSave(), nullptr);
    const auto actual = manifest(target); EXPECT_EQ(actual.snapshotId, controller.unconfirmedSave()->snapshot.snapshotId);
    EXPECT_TRUE(QFileInfo::exists(datasetPath(actual, target)));
    EXPECT_EQ(controller.saveProject().failure->category, CommandError::UnsafeState);
}

TEST_F(Save, FailedCommitLeavesPreviousManifestAndBaseline)
{
    saved(); const auto prior = bytes(target); const auto baseline = controller.savedProject()->snapshot.snapshotId;
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    SaveTestAccess::configure(controller, {}, {}, [](QSaveFile &) { return false; });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_EQ(bytes(target), prior);
    EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, baseline); EXPECT_TRUE(controller.isDirty());
}

TEST_F(Save, WindowsSharingViolationDuringRealCommitPreservesPreviousManifest)
{
    saved(); const auto prior = bytes(target); const auto baseline = controller.savedProject()->snapshot.snapshotId;
    ASSERT_EQ(controller.setCell(.5).outcome, CommandOutcome::Applied);
    struct FileLease {
        HANDLE handle;
        ~FileLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } lease{CreateFileW(QDir::toNativeSeparators(target).toStdWString().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    ASSERT_NE(lease.handle, INVALID_HANDLE_VALUE);
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    ASSERT_TRUE(controller.operation().failure->saveError);
    EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::PublicationUncertain);
    EXPECT_EQ(bytes(target), prior); EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, baseline);
    EXPECT_TRUE(controller.isDirty()); EXPECT_NE(controller.unconfirmedSave(), nullptr); noStaging();
}

TEST_F(Save, ConcurrentControllersSerializeAndRejectStaleSnapshot)
{
    saved(); const auto initial = SaveTestAccess::saved(controller);
    ProjectController second(area.path() + QStringLiteral("/second"));
    SaveTestAccess::installFixture(second, *controller.project(), initial);
    Event committing, proceed;
    SaveTestAccess::configure(controller, [&](SavePhase p) {
        if (p == SavePhase::BeforeCommit) { committing.set(); proceed.wait(); }
    });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ready();
    OperationTestAccess::ready(controller, controller.operation().id); committing.wait();
    ASSERT_EQ(second.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(second));
    EXPECT_EQ(second.operation().failure->saveError->code, SaveErrorCode::Locked);
    proceed.set(); ASSERT_TRUE(finish(controller));
    const auto prior = bytes(target);
    ASSERT_EQ(second.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(second));
    EXPECT_EQ(second.operation().failure->saveError->code, SaveErrorCode::Conflict); EXPECT_EQ(bytes(target), prior);
}

TEST_F(Save, ConcurrentProcessesUseTheSameDirectoryLock)
{
    QProcess child;
    child.start(QString::fromUtf8(SAVE_CRASH_PROBE_EXE), {source, target, QStringLiteral("hold")});
    ASSERT_TRUE(child.waitForStarted());
    const bool waiting = spinUntil([&] { return QFileInfo::exists(target + QStringLiteral(".ready")) || child.state() == QProcess::NotRunning; });
    if (!waiting || child.state() == QProcess::NotRunning) {
        child.kill(); child.waitForFinished(); FAIL() << "Child did not reach commit barrier";
    }
    EXPECT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied);
    EXPECT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    if (controller.operation().failure && controller.operation().failure->saveError)
        EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::Locked);
    EXPECT_FALSE(QFileInfo::exists(target));
    child.kill(); EXPECT_TRUE(child.waitForFinished());
    EXPECT_FALSE(QFileInfo::exists(target));
}

TEST_F(Save, SaveAsAcrossAvailableWindowsVolumes)
{
    saved(); const auto first = controller.savedProject()->datasetIdentity;
    QTemporaryDir other(QString::fromUtf8(SAVE_TEST_WORKSPACE) + QStringLiteral("/save-cross-volume-XXXXXX"));
    if (!other.isValid()) GTEST_SKIP() << "Workspace test directory unavailable";
    const auto path = other.filePath(QStringLiteral("project/P.pc2dproj"));
    ASSERT_EQ(controller.saveProjectAs({path}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    ASSERT_TRUE(first); ASSERT_TRUE(controller.savedProject()->datasetIdentity);
    if (first->volume == controller.savedProject()->datasetIdentity->volume) GTEST_SKIP() << "Same physical volume";
    EXPECT_EQ(bytes(datasetPath(manifest(target), target)), bytes(cachePath()));
    EXPECT_EQ(controller.savedProject()->manifestPath, path); EXPECT_FALSE(controller.isDirty());
}

TEST_F(Save, ReparseDestinationAndInvalidPathAreRejected)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(target).absolutePath()));
    QProcess link;
    const auto outside = area.path() + QStringLiteral("/outside"); ASSERT_TRUE(QDir().mkpath(outside));
    link.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
        QDir::toNativeSeparators(QFileInfo(target).absolutePath() + QStringLiteral("/data")), QDir::toNativeSeparators(outside)});
    ASSERT_TRUE(link.waitForFinished()); ASSERT_EQ(link.exitCode(), 0);
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_FALSE(QFileInfo::exists(target));
    EXPECT_TRUE(QDir(outside).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    ASSERT_EQ(controller.saveProject({QStringLiteral("relative.pc2dproj")}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller)); EXPECT_EQ(controller.operation().failure->saveError->code, SaveErrorCode::InvalidTarget);
}

TEST_F(Save, DiskFullAndReadErrorsPreservePriorSave)
{
    saved(); const auto prior = bytes(target);
    for (const auto phase : {SavePhase::CopyRead, SavePhase::CopyWrite}) {
        const auto other = area.path() + QStringLiteral("/error-%1/P.pc2dproj").arg(int(phase));
        SaveTestAccess::configure(controller, [phase](SavePhase p) {
            if (p == phase) throw SaveError{phase == SavePhase::CopyWrite
                ? SaveErrorCode::ResourceUnavailable : SaveErrorCode::FileIO, p,
                std::uint32_t(phase == SavePhase::CopyWrite ? ERROR_DISK_FULL : ERROR_READ_FAULT)};
        });
        ASSERT_EQ(controller.saveProjectAs({other}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_EQ(bytes(target), prior);
        EXPECT_FALSE(QFileInfo::exists(other)); EXPECT_EQ(controller.operation().failure->saveError->phase, phase);
    }
}

TEST_F(Save, DestructorAfterPermissionJoinsWithoutReceiptDelivery)
{
    auto other = std::make_unique<ProjectController>(area.path() + QStringLiteral("/other"));
    SaveTestAccess::installFixture(*other, *controller.project());
    Event entered, proceed;
    SaveTestAccess::configure(*other, [&](SavePhase p) {
        if (p == SavePhase::BeforeCommit) { entered.set(); proceed.wait(); }
    });
    ASSERT_EQ(other->saveProject({target}).outcome, CommandOutcome::Applied);
    auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(*other));
    ASSERT_TRUE(waitContext(context, true)); OperationTestAccess::ready(*other, other->operation().id); entered.wait();
    std::thread release([&] { proceed.set(); });
    other.reset(); release.join();
    EXPECT_TRUE(context->nativeCompletion.finished());
    const auto s = manifest(target); EXPECT_TRUE(QFileInfo::exists(datasetPath(s, target)));
}

class SaveFault : public Save, public testing::WithParamInterface<SavePhase> {};
TEST_P(SaveFault, FailureKeepsBaselineAndPreviousManifest)
{
    saved(); const auto prior = bytes(target); const auto baseline = controller.savedProject()->snapshot.snapshotId;
    const auto location = controller.project()->dataset()->location;
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    const auto phase = GetParam();
    // Save As exercises every copy phase while the original project remains intact.
    const auto other = area.path() + QStringLiteral("/fault/P.pc2dproj");
    SaveTestAccess::configure(controller, [phase](SavePhase p) {
        if (p == phase) throw std::runtime_error("injected save phase failure");
    });
    ASSERT_EQ(controller.saveProjectAs({other}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(bytes(target), prior);
    if (phase == SavePhase::Receipt || phase == SavePhase::Cleanup) {
        EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded); EXPECT_TRUE(controller.operation().cleanupWarning);
        const auto s = manifest(other); EXPECT_TRUE(QFileInfo::exists(datasetPath(s, other)));
        EXPECT_FALSE(controller.isDirty());
    } else {
        EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_FALSE(QFileInfo::exists(other));
        EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, baseline);
        EXPECT_EQ(controller.project()->dataset()->location, location); EXPECT_TRUE(controller.isDirty());
        EXPECT_EQ(controller.operation().failure->saveError->phase, phase);
    }
    noStaging();
}
INSTANTIATE_TEST_SUITE_P(Phases, SaveFault, testing::Values(SavePhase::Target, SavePhase::Staging,
    SavePhase::Source, SavePhase::CopyRead, SavePhase::CopyWrite, SavePhase::CopyVerify,
    SavePhase::DatasetPublish, SavePhase::DatasetPublished, SavePhase::ManifestPrepare,
    SavePhase::ManifestWrite, SavePhase::BeforeCommit, SavePhase::ManifestCommit, SavePhase::Receipt, SavePhase::Cleanup));

TEST_F(Save, CrashProcessArtifactsRemainConsistent)
{
    for (const auto phase : {QStringLiteral("copy"), QStringLiteral("dataset"), QStringLiteral("commit"), QStringLiteral("receipt")}) {
        const auto path = area.path() + u'/' + phase + QStringLiteral("/P.pc2dproj");
        QProcess child;
        child.start(QString::fromUtf8(SAVE_CRASH_PROBE_EXE), {source, path, phase});
        ASSERT_TRUE(child.waitForStarted()); ASSERT_TRUE(child.waitForFinished(20000));
        ASSERT_EQ(child.exitCode(), 73) << child.readAllStandardError().constData();
        if (phase == u"receipt") {
            ASSERT_TRUE(QFileInfo::exists(path)); const auto s = manifest(path);
            auto r = cache::CacheReader::open(datasetPath(s, path), limits); ASSERT_TRUE(r);
            EXPECT_TRUE(r.value()->validate(limits, area.path()));
        } else EXPECT_FALSE(QFileInfo::exists(path));
        if (phase == u"dataset" || phase == u"commit") {
            QDirIterator it(QFileInfo(path).absolutePath(), {QStringLiteral("*.pc2dcache")}, QDir::Files, QDirIterator::Subdirectories);
            ASSERT_TRUE(it.hasNext()); auto r = cache::CacheReader::open(it.next(), limits); ASSERT_TRUE(r);
            EXPECT_TRUE(r.value()->validate(limits, area.path()));
        }
    }
}

TEST_F(Save, CrashDuringRepeatedSavePreservesCompletedManifest)
{
    for (const auto phase : {QStringLiteral("commit"), QStringLiteral("receipt")}) {
        const auto path = area.path() + QStringLiteral("/repeat-") + phase + QStringLiteral("/P.pc2dproj");
        QProcess child;
        child.start(QString::fromUtf8(SAVE_CRASH_PROBE_EXE), {source, path, phase, QStringLiteral("repeat")});
        ASSERT_TRUE(child.waitForStarted()); ASSERT_TRUE(child.waitForFinished(20000)); ASSERT_EQ(child.exitCode(), 73);
        const auto s = manifest(path); EXPECT_DOUBLE_EQ(s.cell, phase == u"receipt" ? .5 : 1.);
        auto r = cache::CacheReader::open(datasetPath(s, path), limits); ASSERT_TRUE(r);
        EXPECT_TRUE(r.value()->validate(limits, area.path()));
    }
}
