#include "operation_test_support.h"
#include "recovery_test_access.h"
#include "save_test_access.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QMap>
#include <QProcess>
#include <QSaveFile>
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <tuple>

#define NOMINMAX
#include <Windows.h>

using namespace cadcontour;
using namespace cadcontour::test;
using namespace cadcontour::persistence;
namespace {
QByteArray bytes(const QString &path)
{ QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
void write(const QString &path, const QByteArray &data)
{ QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write(data), data.size()); }
void expectSameLocation(const ImportedDataset &a, const ImportedDataset &b)
{
    ASSERT_TRUE(a.location); ASSERT_TRUE(b.location);
    EXPECT_EQ(a.location->storageId, b.location->storageId);
    EXPECT_EQ(a.location->storageDirectory, b.location->storageDirectory);
    EXPECT_EQ(a.location->relativeCachePath, b.location->relativeCachePath);
    EXPECT_EQ(a.cacheId, b.cacheId);
}
class Recovery : public testing::Test {
protected:
    QTemporaryDir area;
    std::unique_ptr<ProjectController> controller;
    QString source, savedPath;
    QString root() const { return area.path() + QStringLiteral("/imports/recovery"); }
    QString recordPath() const { return QDir(root()).filePath(recoveryFileName(controller->recoveryRecord()->sessionId)); }
    bool finishBeforeAutosave()
    {
        // finish() pumps events: block timeout delivery before Import/Save can
        // schedule its immediate tick, then stop both triggers before unblocking.
        const auto blocked = RecoveryTestAccess::blockAutosave(*controller);
        const auto completed = finish(*controller);
        RecoveryTestAccess::timers(*controller);
        return completed;
    }
    QMap<QString, QByteArray> datasetDigests() const
    {
        QMap<QString, QByteArray> result;
        QDirIterator files(area.path(), {"*.pc2dcache"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto path = files.next();
            result.insert(path, QCryptographicHash::hash(bytes(path), QCryptographicHash::Sha256));
        }
        return result;
    }
    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        source = area.path() + QStringLiteral("/source.xyz"); savedPath = area.path() + QStringLiteral("/saved/project.pc2dproj");
        write(source, "1 2 0\n3 4 0\n");
        controller = std::make_unique<ProjectController>(area.path() + QStringLiteral("/imports"));
        ASSERT_EQ(controller->importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finishBeforeAutosave()); ASSERT_EQ(controller->operation().status, OperationStatus::Succeeded)
            << (controller->operation().failure && controller->operation().failure->recoveryError
                ? controller->operation().failure->recoveryError->detail.toStdString() : "no recovery diagnostic");
        ASSERT_EQ(controller->operation().kind, OperationKind::Import);
        ASSERT_FALSE(controller->recoveryRecord());
    }
    void TearDown() override
    {
        if (controller) {
            RecoveryTestAccess::timers(*controller);
            fault({});
            RecoveryTestAccess::acceptance(*controller, {});
            controller.reset(); // drain while the fixture's storage still exists
        }
    }
    void autosave()
    {
        ASSERT_EQ(controller->autosave().outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(*controller)); ASSERT_EQ(controller->operation().status, OperationStatus::Succeeded)
            << (controller->operation().failure && controller->operation().failure->recoveryError
                ? controller->operation().failure->recoveryError->detail.toStdString() : "no recovery diagnostic");
        ASSERT_TRUE(controller->recoveryRecord());
        ASSERT_TRUE(matchesSavedContent(*controller->project(), *controller->recoveryRecord()->state));
        const auto persisted = candidate();
        ASSERT_TRUE(persisted.record);
        EXPECT_EQ(persisted.record->digest, controller->recoveryRecord()->digest);
        EXPECT_EQ(persisted.record->state->snapshot.snapshotId, controller->recoveryRecord()->state->snapshot.snapshotId);
    }
    void save()
    {
        ASSERT_EQ(controller->saveProject({savedPath}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finishBeforeAutosave()); ASSERT_EQ(controller->operation().status, OperationStatus::Succeeded);
        ASSERT_EQ(controller->operation().kind, OperationKind::Save);
    }
    RecoveryCandidate candidate()
    {
        auto found = discoverRecovery(root());
        EXPECT_TRUE(found); if (!found || found.value().size() != 1) return {};
        return found.value()[0];
    }
    void restart()
    {
        controller.reset();
        controller = std::make_unique<ProjectController>(area.path() + QStringLiteral("/imports"));
        RecoveryTestAccess::timers(*controller);
    }
    void restore(RecoveryCandidate c)
    {
        ASSERT_EQ(controller->restoreRecovery({std::move(c)}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(*controller)); ASSERT_EQ(controller->operation().status, OperationStatus::Succeeded);
    }
    void fault(std::shared_ptr<RecoveryTransactionTestAccess::Hooks> hooks)
    { RecoveryTestAccess::configure(*controller, std::move(hooks)); }
};

TEST_F(Recovery, FirstImportAutomaticallyPublishesAfterNativeCompletion)
{
    // Re-enable the immediate trigger via a real second import; no manual autosave.
    ImportRequest request{source, {8ULL << 20, 64ULL << 20, 8}}; request.replacementConfirmed = true;
    ASSERT_EQ(controller->importAscXyz(request).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller);
        return controller->recoveryRecord() && !controller->operation().active; }));
    EXPECT_TRUE(controller->isDirty()); EXPECT_FALSE(controller->savedProject());
    EXPECT_EQ(candidate().session, RecoverySessionState::Active);
}

class RecoveryScheduling : public Recovery, public testing::WithParamInterface<std::tuple<bool, bool>> {};
TEST_P(RecoveryScheduling, ExactSnapshotNoChangeAndNewEditReallyPublishes)
{
    const auto [afterSave, automaticCompleted] = GetParam();
    if (afterSave) {
        ASSERT_EQ(controller->saveProject({savedPath}).outcome, CommandOutcome::Applied);
    } else {
        ImportRequest request{source, {8ULL << 20, 64ULL << 20, 8}}; request.replacementConfirmed = true;
        ASSERT_EQ(controller->importAscXyz(request).outcome, CommandOutcome::Applied);
    }
    if (automaticCompleted) {
        ASSERT_TRUE(spinUntil([&] {
            OperationTestAccess::poll(*controller);
            return controller->operation().kind == OperationKind::Autosave
                && !controller->operation().active && controller->recoveryRecord();
        }));
        ASSERT_EQ(controller->operation().status, OperationStatus::Succeeded);
        RecoveryTestAccess::timers(*controller);
    } else {
        ASSERT_TRUE(finishBeforeAutosave());
        ASSERT_EQ(controller->operation().kind, afterSave ? OperationKind::Save : OperationKind::Import);
        ASSERT_EQ(controller->operation().status, OperationStatus::Succeeded);
        ASSERT_FALSE(controller->recoveryRecord());
        const auto found = discoverRecovery(root());
        ASSERT_TRUE(found); ASSERT_TRUE(found.value().empty());
        ASSERT_NO_FATAL_FAILURE(autosave()); // first publication must perform real I/O
    }

    const auto initial = candidate();
    ASSERT_TRUE(initial.record); ASSERT_TRUE(initial.record->state);
    EXPECT_EQ(initial.session, RecoverySessionState::Active);
    EXPECT_EQ(initial.relation, afterSave ? RecoveryRelation::MatchesExplicit : RecoveryRelation::Unsaved);
    EXPECT_TRUE(matchesSavedContent(*controller->project(), *initial.record->state));
    EXPECT_DOUBLE_EQ(initial.record->state->snapshot.cell, 1);
    ASSERT_NO_FATAL_FAILURE(expectSameLocation(initial.record->state->content, *controller->project()->dataset()));
    ASSERT_EQ(bool(initial.record->baseline), afterSave);
    if (afterSave) {
        EXPECT_EQ(initial.record->baseline->manifestPath, savedPath);
        EXPECT_EQ(initial.record->baseline->manifestDigest, controller->savedProject()->manifestDigest);
    }
    const auto before = bytes(recordPath()), explicitBytes = bytes(savedPath);
    const auto datasets = datasetDigests(); ASSERT_FALSE(datasets.isEmpty());
    const auto operationId = controller->operation().id;
    const auto phases = std::make_shared<std::vector<RecoveryPhase>>();
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [phases](RecoveryPhase phase) { phases->push_back(phase); };
    fault(hooks); // all automatic/initial publication has completed before observation
    EXPECT_EQ(controller->autosave().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller->operation().id, operationId);
    EXPECT_EQ(bytes(recordPath()), before); EXPECT_TRUE(phases->empty());

    ASSERT_EQ(controller->setCell(.125).outcome, CommandOutcome::Applied);
    const auto revision = controller->project()->inputRevision();
    ASSERT_NO_FATAL_FAILURE(autosave());
    EXPECT_NE(controller->operation().id, operationId);
    EXPECT_EQ(std::count(phases->begin(), phases->end(), RecoveryPhase::BeforeCommit), 1);
    EXPECT_EQ(std::count(phases->begin(), phases->end(), RecoveryPhase::Published), 1);
    const auto updated = candidate(); ASSERT_TRUE(updated.record);
    EXPECT_TRUE(matchesSavedContent(*controller->project(), *updated.record->state));
    EXPECT_DOUBLE_EQ(updated.record->state->snapshot.cell, .125);
    EXPECT_NE(updated.record->state->snapshot.snapshotId, initial.record->state->snapshot.snapshotId);
    EXPECT_NE(bytes(recordPath()), before);
    ASSERT_NO_FATAL_FAILURE(expectSameLocation(updated.record->state->content, initial.record->state->content));
    ASSERT_EQ(bool(updated.record->baseline), afterSave);
    if (afterSave) {
        EXPECT_EQ(updated.record->baseline->manifestDigest, initial.record->baseline->manifestDigest);
        EXPECT_EQ(updated.record->baseline->manifestPath, initial.record->baseline->manifestPath);
    }
    EXPECT_EQ(bytes(savedPath), explicitBytes); EXPECT_EQ(datasetDigests(), datasets);
    EXPECT_EQ(controller->project()->inputRevision(), revision);
    EXPECT_TRUE(controller->isDirty()); EXPECT_TRUE(controller->canUndo()); EXPECT_FALSE(controller->canRedo());
}
INSTANTIATE_TEST_SUITE_P(ImportAndSaveOrders, RecoveryScheduling, testing::Combine(testing::Bool(), testing::Bool()),
    [](const auto &info) { return std::string(std::get<0>(info.param) ? "AfterSave" : "AfterImport")
        + (std::get<1>(info.param) ? "AutomaticCompleted" : "BeforeAutomatic"); });
TEST_F(Recovery, ExactCellUnsavedRestoreWithNewRuntimeAndEmptyHistory)
{
    const auto id = controller->project()->projectId();
    const auto cell = std::nextafter(0.1, 1.0);
    controller->setCell(cell); autosave();
    const auto cachePath = controller->project()->dataset()->cachePath();
    restart(); const auto found = candidate();
    EXPECT_EQ(found.session, RecoverySessionState::Closed); EXPECT_EQ(found.relation, RecoveryRelation::Unsaved);
    restore(found);
    EXPECT_EQ(controller->project()->projectId(), id);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(controller->project()->cell()), std::bit_cast<std::uint64_t>(cell));
    EXPECT_EQ(controller->project()->inputRevision(), 0); EXPECT_FALSE(controller->canUndo()); EXPECT_FALSE(controller->canRedo());
    EXPECT_TRUE(controller->isDirty()); EXPECT_FALSE(controller->savedProject());
    EXPECT_EQ(controller->project()->dataset()->cachePath(), cachePath);
    EXPECT_EQ(controller->pointCacheAvailability()->access, CacheAccess::AvailableLimited);
    EXPECT_EQ(controller->pointCacheAvailability()->reads.pointPages, 0);
    EXPECT_EQ(controller->pointCacheAvailability()->reads.readBytes, 73728);
    RecordProperty("limitedRecoveryReadBytes", 73728);
}
TEST_F(Recovery, NoRewriteForUnchangedPersistentContentOrUndoBackToRecovery)
{
    autosave(); const auto path = recordPath(); const auto before = bytes(path);
    const auto id = controller->operation().id;
    EXPECT_EQ(controller->autosave().outcome, CommandOutcome::NoChange);
    controller->setCell(.5); controller->undo();
    EXPECT_GT(controller->project()->inputRevision(), 0);
    EXPECT_EQ(controller->autosave().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(bytes(path), before); EXPECT_EQ(controller->operation().id, id);
}
TEST_F(Recovery, PeriodicTimerSavesChangedAppliedState)
{
    autosave(); controller->setCell(.125); RecoveryTestAccess::timers(*controller, 5);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller);
        return controller->recoveryRecord()->state->snapshot.cell == .125 && !controller->operation().active; }));
    RecoveryTestAccess::timers(*controller);
}
TEST_F(Recovery, UndoToOlderSnapshotAfterNewAutosaveIsSaved)
{
    autosave(); controller->setCell(.5); autosave(); controller->undo(); autosave();
    EXPECT_DOUBLE_EQ(controller->recoveryRecord()->state->snapshot.cell, 1);
}
TEST_F(Recovery, CellChangeDuringWritePublishesCapturedStateAndKeepsLaterEdit)
{
    autosave(); controller->setCell(.5);
    Event entered, release;
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase phase) { if (phase == RecoveryPhase::BeforeCommit) { entered.set(); release.wait(); } };
    fault(hooks); controller->autosave(); entered.wait();
    controller->setCell(.25); release.set(); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Succeeded);
    EXPECT_DOUBLE_EQ(controller->recoveryRecord()->state->snapshot.cell, .5);
    EXPECT_DOUBLE_EQ(controller->project()->cell(), .25); EXPECT_TRUE(controller->isDirty());
    fault({}); autosave(); EXPECT_DOUBLE_EQ(controller->recoveryRecord()->state->snapshot.cell, .25);
}
TEST_F(Recovery, BusyTickDoesNotQueueSnapshot)
{
    autosave(); controller->setCell(.5); const auto before = bytes(recordPath());
    ControlledWork work;
    ASSERT_EQ(OperationTestAccess::start(*controller, work.function()).outcome, CommandOutcome::Applied); work.entered.wait();
    EXPECT_EQ(controller->autosave().failure->category, CommandError::Busy);
    work.complete.set(); ASSERT_TRUE(finish(*controller)); QCoreApplication::processEvents();
    EXPECT_FALSE(controller->operation().active); EXPECT_EQ(bytes(recordPath()), before);
}
TEST_F(Recovery, AutosaveDoesNotChangeExplicitBaselineDirtyOrUndo)
{
    save(); const auto digest = controller->savedProject()->manifestDigest;
    controller->setCell(.5); autosave();
    EXPECT_TRUE(controller->isDirty()); EXPECT_EQ(controller->savedProject()->manifestDigest, digest);
    controller->undo(); EXPECT_FALSE(controller->isDirty()); autosave();
    EXPECT_FALSE(controller->isDirty()); EXPECT_TRUE(controller->canRedo());
}
TEST_F(Recovery, ExistingSaveRestoresDirtyRelativeToOriginalBaselineAndCanSave)
{
    save(); controller->setCell(.5); autosave(); restart();
    EXPECT_EQ(candidate().relation, RecoveryRelation::Modified); restore(candidate());
    EXPECT_TRUE(controller->isDirty()); ASSERT_TRUE(controller->savedProject());
    EXPECT_DOUBLE_EQ(controller->savedProject()->snapshot.cell, 1);
    ASSERT_EQ(controller->saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Succeeded); EXPECT_FALSE(controller->isDirty());
}
TEST_F(Recovery, MatchingExplicitRecoveryRestoresClean)
{
    save(); autosave(); restart(); EXPECT_EQ(candidate().relation, RecoveryRelation::MatchesExplicit);
    restore(candidate()); EXPECT_FALSE(controller->isDirty()); EXPECT_FALSE(controller->recoveryRequiresSaveAs());
}
TEST_F(Recovery, ExplicitSaveReconcilesRecoveryWithoutDestroyingOldRecord)
{
    autosave(); const auto old = bytes(recordPath()); controller->setCell(.5); save();
    EXPECT_FALSE(old.isEmpty()); autosave();
    EXPECT_EQ(candidate().relation, RecoveryRelation::MatchesExplicit);
    EXPECT_FALSE(controller->isDirty());
}
TEST_F(Recovery, FailedExplicitSaveKeepsRecoveryAndBaseline)
{
    save(); autosave(); const auto old = bytes(recordPath()); controller->setCell(.5);
    SaveTestAccess::configure(*controller, {}, {}, [](QSaveFile &) { return false; });
    controller->saveProject(); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Failed); EXPECT_EQ(bytes(recordPath()), old);
    EXPECT_DOUBLE_EQ(controller->savedProject()->snapshot.cell, 1); EXPECT_TRUE(controller->isDirty());
}
TEST_F(Recovery, IndependentExplicitChangeRestoresDetachedAndRequiresSaveAs)
{
    save(); controller->setCell(.5); autosave(); restart();
    ProjectController other(area.path() + QStringLiteral("/other"));
    ASSERT_EQ(other.openProject({savedPath}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(other));
    other.setCell(.25); other.saveProject(); ASSERT_TRUE(finish(other)); RecoveryTestAccess::timers(other);
    const auto explicitBytes = bytes(savedPath);
    EXPECT_EQ(candidate().relation, RecoveryRelation::Conflict); restore(candidate());
    EXPECT_TRUE(controller->recoveryRequiresSaveAs()); EXPECT_TRUE(controller->isDirty()); EXPECT_FALSE(controller->savedProject());
    EXPECT_EQ(controller->saveProject().outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller->saveProject({savedPath}).outcome, CommandOutcome::Rejected);
    ASSERT_EQ(controller->saveProjectAs({area.path() + "/separate/project.pc2dproj"}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(*controller)); EXPECT_EQ(controller->operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(bytes(savedPath), explicitBytes); EXPECT_FALSE(controller->recoveryRequiresSaveAs());
}
TEST_F(Recovery, MissingExplicitSaveIsConflict)
{
    save(); controller->setCell(.5); autosave(); restart(); ASSERT_TRUE(QFile::remove(savedPath));
    EXPECT_EQ(candidate().relation, RecoveryRelation::Conflict); restore(candidate());
    EXPECT_TRUE(controller->recoveryRequiresSaveAs());
}
TEST_F(Recovery, ConflictSurvivesAutosaveRestartUndoAndFailedSaveAs)
{
    save(); controller->setCell(.5); autosave(); restart();
    ProjectController other(area.path() + "/other");
    ASSERT_EQ(other.openProject({savedPath}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(other));
    other.setCell(.25); ASSERT_EQ(other.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(other));
    RecoveryTestAccess::timers(other);
    const auto explicitBytes = bytes(savedPath);
    restore(candidate());
    ASSERT_TRUE(controller->recoveryRequiresSaveAs());
    controller->setCell(.125); autosave(); restart(); restore(candidate());
    EXPECT_TRUE(controller->recoveryRequiresSaveAs());
    EXPECT_TRUE(controller->isDirty()); EXPECT_FALSE(controller->savedProject());
    EXPECT_EQ(controller->saveProject().outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller->saveProject({savedPath}).outcome, CommandOutcome::Rejected);
    controller->setCell(.0625); autosave(); controller->undo(); autosave(); restart(); restore(candidate());
    EXPECT_TRUE(controller->recoveryRequiresSaveAs()); EXPECT_DOUBLE_EQ(controller->project()->cell(), .125);
    // Even matching the independently saved content does not resolve an established conflict.
    controller->setCell(.25); autosave(); restart(); restore(candidate());
    EXPECT_TRUE(controller->recoveryRequiresSaveAs()); EXPECT_FALSE(controller->savedProject());
    const auto destination = area.path() + "/resolved/project.pc2dproj";
    SaveTestAccess::configure(*controller, {}, [](QIODevice &, const char *, qint64) { return qint64(-1); });
    ASSERT_EQ(controller->saveProjectAs({area.path() + "/failed/project.pc2dproj"}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Failed);
    EXPECT_TRUE(controller->recoveryRequiresSaveAs()); EXPECT_FALSE(controller->savedProject());
    controller->setCell(.125); autosave(); restart(); restore(candidate());
    EXPECT_TRUE(controller->recoveryRequiresSaveAs());
    ASSERT_EQ(controller->saveProjectAs({destination}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finishBeforeAutosave());
    ASSERT_EQ(controller->operation().status, OperationStatus::Succeeded); RecoveryTestAccess::timers(*controller);
    EXPECT_FALSE(controller->recoveryRequiresSaveAs()); EXPECT_FALSE(controller->isDirty());
    ASSERT_TRUE(controller->savedProject()); EXPECT_EQ(controller->savedProject()->manifestPath, destination);
    EXPECT_EQ(controller->project()->dataset()->location, controller->savedProject()->content.location);
    autosave(); restart(); restore(candidate());
    EXPECT_FALSE(controller->recoveryRequiresSaveAs()); EXPECT_FALSE(controller->isDirty());
    controller->setCell(.5); autosave(); restart(); restore(candidate());
    EXPECT_FALSE(controller->recoveryRequiresSaveAs()); EXPECT_TRUE(controller->isDirty());
    ASSERT_EQ(controller->saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Succeeded); EXPECT_FALSE(controller->isDirty());
    EXPECT_EQ(bytes(savedPath), explicitBytes);
}

class RecoveryLegacy : public Recovery, public testing::WithParamInterface<int> {};
TEST_P(RecoveryLegacy, ReadsVersionOneAndUpgradesWithoutLosingBaseline)
{
    if (GetParam() != 0) save();
    controller->setCell(.5); autosave();
    const auto path = recordPath(); restart();
    auto parts = bytes(path).split('\n');
    parts[0] = "PC2D-RECOVERY 1"; parts.removeAt(9);
    parts.removeLast(); parts.removeLast();
    auto legacy = parts.join('\n') + '\n';
    legacy += QCryptographicHash::hash(legacy, QCryptographicHash::Sha256).toHex() + '\n';
    write(path, legacy);
    if (GetParam() == 2) ASSERT_TRUE(QFile::remove(savedPath));
    const auto c = candidate();
    EXPECT_EQ(c.relation, GetParam() == 0 ? RecoveryRelation::Unsaved
        : GetParam() == 1 ? RecoveryRelation::Modified : RecoveryRelation::Conflict);
    EXPECT_EQ(bytes(path), legacy); // discovery does not migrate or rewrite
    restore(c);
    EXPECT_TRUE(bytes(path).startsWith("PC2D-RECOVERY 2\n"));
    EXPECT_DOUBLE_EQ(controller->project()->cell(), .5);
    EXPECT_EQ(controller->recoveryRequiresSaveAs(), GetParam() == 2);
    EXPECT_EQ(bool(controller->savedProject()), GetParam() == 1);
    controller->setCell(.125); autosave(); restart(); restore(candidate());
    EXPECT_EQ(controller->recoveryRequiresSaveAs(), GetParam() == 2);
    EXPECT_EQ(bool(controller->savedProject()), GetParam() == 1);
}
INSTANTIATE_TEST_SUITE_P(UnsavedModifiedConflict, RecoveryLegacy, testing::Range(0, 3));
TEST_F(Recovery, RejectIsDurableAndRetainsUnprovenDatasetOwnership)
{
    autosave(); const auto cachePath = QString::fromStdWString(controller->project()->dataset()->cachePath().wstring());
    restart(); const auto c = candidate();
    controller->rejectRecovery(c); ASSERT_TRUE(finish(*controller)); EXPECT_EQ(controller->operation().status, OperationStatus::Succeeded);
    restart(); EXPECT_EQ(candidate().session, RecoverySessionState::Rejected); EXPECT_TRUE(QFile::exists(cachePath));
    controller->restoreRecovery({candidate()}); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Failed); EXPECT_FALSE(controller->project());
}
TEST_F(Recovery, ActiveSessionCannotBeRestoredOrRejected)
{
    autosave(); const auto c = candidate(); EXPECT_EQ(c.session, RecoverySessionState::Active);
    EXPECT_FALSE(persistence::restoreRecovery(root(), c, {}, {1ULL << 20, 0, 2}));
    EXPECT_FALSE(persistence::rejectRecovery(root(), c)); EXPECT_TRUE(controller->project());
}
TEST_F(Recovery, DiscoveryUsesControllerWorkerAndDoesNotReplaceProject)
{
    autosave(); const auto state = controller->project();
    ASSERT_EQ(controller->discoverRecovery().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(*controller));
    ASSERT_EQ(controller->recoveryCandidates().size(), 1); EXPECT_EQ(controller->project(), state);
}

class RecoveryPartial : public Recovery, public testing::WithParamInterface<bool> {};
TEST_P(RecoveryPartial, UnavailableCacheRestoresMetadataAndCanRecheck)
{
    controller->setCell(.125); autosave();
    const auto cachePath = QString::fromStdWString(controller->project()->dataset()->cachePath().wstring());
    const auto original = bytes(cachePath); restart();
    if (GetParam()) write(cachePath, "broken"); else ASSERT_TRUE(QFile::remove(cachePath));
    restore(candidate()); EXPECT_DOUBLE_EQ(controller->project()->cell(), .125); EXPECT_TRUE(controller->isDirty());
    EXPECT_EQ(controller->pointCacheAvailability()->access, GetParam() ? CacheAccess::Corrupt : CacheAccess::Missing);
    EXPECT_EQ(controller->setCell(.5).outcome, CommandOutcome::Rejected);
    write(cachePath, original);
    ASSERT_EQ(controller->recheckPointCache().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->pointCacheAvailability()->access, CacheAccess::AvailableLimited);
    EXPECT_TRUE(controller->isDirty()); EXPECT_FALSE(controller->savedProject());
}
INSTANTIATE_TEST_SUITE_P(MissingAndCorrupt, RecoveryPartial, testing::Bool());

class RecoveryFault : public Recovery, public testing::WithParamInterface<int> {};
TEST_P(RecoveryFault, FailedUpdatePreservesPreviousRecordAndAllDatasets)
{
    ASSERT_NO_FATAL_FAILURE(save());
    ASSERT_NO_FATAL_FAILURE(autosave());
    const auto before = bytes(recordPath()), explicitBytes = bytes(savedPath);
    const auto previous = candidate(); ASSERT_TRUE(previous.record); ASSERT_TRUE(previous.record->baseline);
    const auto datasets = datasetDigests(); ASSERT_EQ(datasets.size(), 2); // imported + explicit copy
    const auto cachePath = QString::fromStdWString(controller->project()->dataset()->cachePath().wstring());
    ASSERT_EQ(controller->setCell(.5).outcome, CommandOutcome::Applied);
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    struct Observation { std::vector<RecoveryPhase> phases; bool injected = false; };
    const auto observed = std::make_shared<Observation>();
    const auto faultId = GetParam();
    hooks->phase = [faultId, observed](RecoveryPhase p) {
        observed->phases.push_back(p);
        if (faultId < 2 && p == (faultId == 0 ? RecoveryPhase::Target : RecoveryPhase::Open)) {
            observed->injected = true;
            throw Error{ErrorCode::FileIO, {}, QStringLiteral("Injected open/target failure")};
        }
    };
    if (faultId == 2 || faultId == 3 || faultId == 6) hooks->write = [faultId, observed](QIODevice &f, const char *b, qint64 n) {
        observed->injected = true;
        if (faultId == 3) return f.write(b, n / 2);
        if (faultId == 6) SetLastError(ERROR_DISK_FULL);
        return qint64(-1);
    };
    if (faultId == 4) hooks->flush = [observed](QFileDevice &) { observed->injected = true; return false; };
    if (faultId == 5) hooks->commit = [observed](QSaveFile &) { observed->injected = true; return false; };
    fault(hooks);
    ASSERT_EQ(controller->autosave().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(*controller));
    ASSERT_EQ(controller->operation().status, OperationStatus::Failed);
    ASSERT_TRUE(controller->operation().failure); EXPECT_TRUE(controller->operation().failure->recoveryError);
    EXPECT_TRUE(observed->injected); ASSERT_FALSE(observed->phases.empty());
    const auto expectedPhase = faultId == 0 ? RecoveryPhase::Target : faultId == 1 ? RecoveryPhase::Open
        : faultId == 4 ? RecoveryPhase::Flush : faultId == 5 ? RecoveryPhase::BeforeCommit : RecoveryPhase::Write;
    EXPECT_EQ(observed->phases.back(), expectedPhase);
    EXPECT_EQ(std::count(observed->phases.begin(), observed->phases.end(), RecoveryPhase::Published), 0);
    EXPECT_EQ(bytes(recordPath()), before); EXPECT_EQ(bytes(savedPath), explicitBytes); EXPECT_TRUE(QFile::exists(cachePath));
    EXPECT_EQ(datasetDigests(), datasets);
    const auto retained = candidate(); ASSERT_TRUE(retained.record);
    EXPECT_EQ(retained.record->digest, previous.record->digest);
    EXPECT_EQ(retained.record->state->snapshot.snapshotId, previous.record->state->snapshot.snapshotId);
    EXPECT_DOUBLE_EQ(retained.record->state->snapshot.cell, 1);
    ASSERT_NO_FATAL_FAILURE(expectSameLocation(retained.record->state->content, previous.record->state->content));
    ASSERT_TRUE(retained.record->baseline);
    EXPECT_EQ(retained.record->baseline->manifestDigest, previous.record->baseline->manifestDigest);
    EXPECT_DOUBLE_EQ(controller->project()->cell(), .5); EXPECT_TRUE(controller->isDirty());
}
INSTANTIATE_TEST_SUITE_P(FileFailures, RecoveryFault, testing::Range(0, 7));

TEST_F(Recovery, RealExclusiveRecordLockPreservesOldRecordOnPublicationFailure)
{
    autosave(); const auto path = recordPath(); const auto before = bytes(path);
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    controller->setCell(.5); controller->autosave(); const auto finished = finish(*controller); CloseHandle(handle);
    ASSERT_TRUE(finished); EXPECT_EQ(controller->operation().status, OperationStatus::Failed); EXPECT_EQ(bytes(path), before);
}
TEST_F(Recovery, CleanupFailureAfterPublicationIsWarningAndKeepsNewSnapshot)
{
    autosave(); controller->setCell(.5); auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [](RecoveryPhase p) { if (p == RecoveryPhase::Cleanup) throw std::runtime_error("cleanup fault"); };
    fault(hooks); autosave(); EXPECT_TRUE(controller->operation().cleanupWarning);
    EXPECT_DOUBLE_EQ(candidate().record->state->snapshot.cell, .5);
}
TEST_F(Recovery, CancellationBeforePermissionKeepsOldSnapshot)
{
    autosave(); const auto before = bytes(recordPath()); controller->setCell(.5);
    Event entered, release; auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase p) { if (p == RecoveryPhase::BeforeCommit) { entered.set(); release.wait(); } };
    fault(hooks); controller->autosave(); entered.wait(); EXPECT_EQ(controller->cancelOperation().outcome, CommandOutcome::Applied);
    release.set(); ASSERT_TRUE(finish(*controller)); EXPECT_EQ(controller->operation().status, OperationStatus::Cancelled);
    EXPECT_EQ(bytes(recordPath()), before);
}
TEST_F(Recovery, UserSavePreemptsAutosaveBeforePermissionAndRetriesAfterJoin)
{
    autosave(); controller->setCell(.5); Event entered, release;
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase p) { if (p == RecoveryPhase::BeforeCommit) { entered.set(); release.wait(); } };
    fault(hooks); controller->autosave(); entered.wait();
    EXPECT_EQ(controller->saveProject({savedPath}).failure->category, CommandError::Busy);
    EXPECT_TRUE(controller->operation().cancellationRequested); release.set(); ASSERT_TRUE(finish(*controller));
    save(); EXPECT_FALSE(controller->isDirty());
}
TEST_F(Recovery, StaleSessionResultCannotPublishOrReplaceNewState)
{
    autosave(); const auto before = bytes(recordPath()); controller->setCell(.5);
    Event entered, release; auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase p) { if (p == RecoveryPhase::BeforeCommit) { entered.set(); release.wait(); } };
    fault(hooks); controller->autosave(); entered.wait(); RecoveryTestAccess::epoch(*controller);
    release.set(); ASSERT_TRUE(finish(*controller)); EXPECT_EQ(controller->operation().status, OperationStatus::Discarded);
    EXPECT_EQ(bytes(recordPath()), before); EXPECT_DOUBLE_EQ(controller->project()->cell(), .5);
}
TEST_F(Recovery, ReentrantRecoveryNotificationsRejectCommands)
{
    CommandResult result = CommandResult::noChange();
    QObject::connect(controller.get(), &ProjectController::recoveryChanged, controller.get(), [&] { result = controller->autosave(); });
    autosave(); EXPECT_EQ(result.failure->detail, ErrorDetail::ReentrantCommand);
    EXPECT_EQ(controller->autosave().outcome, CommandOutcome::NoChange);
}
TEST_F(Recovery, LowLevelShutdownKeepsUnsavedRecoveryAndMarksClosed)
{
    autosave(); controller->requestShutdown();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller); return controller->shutdownState() == ShutdownState::Ready; }));
    EXPECT_EQ(candidate().session, RecoverySessionState::Closed); EXPECT_NE(candidate().session, RecoverySessionState::Rejected);
}
TEST_F(Recovery, ConfirmedDiscardIsDurable)
{
    autosave(); controller->requestShutdown(true);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller); return controller->shutdownState() == ShutdownState::Ready; }));
    EXPECT_EQ(candidate().session, RecoverySessionState::Rejected);
}
TEST_F(Recovery, ShutdownDuringWriteCancelsNewSnapshotAndPreservesPrevious)
{
    autosave(); const auto oldCell = controller->recoveryRecord()->state->snapshot.cell; controller->setCell(.5);
    Event entered, release; auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase p) { if (p == RecoveryPhase::Write) { entered.set(); release.wait(); } };
    fault(hooks); controller->autosave(); entered.wait(); controller->requestShutdown();
    EXPECT_EQ(controller->shutdownState(), ShutdownState::Closing); release.set();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller); return controller->shutdownState() == ShutdownState::Ready; }));
    EXPECT_DOUBLE_EQ(candidate().record->state->snapshot.cell, oldCell);
}
TEST_F(Recovery, ShutdownAfterPublicationWaitsForReceiptAndKeepsCapturedSnapshot)
{
    autosave(); controller->setCell(.5); Event entered, release;
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase p) { if (p == RecoveryPhase::Published) { entered.set(); release.wait(); } };
    fault(hooks); controller->autosave();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller); return entered.isSet(); }));
    EXPECT_EQ(controller->cancelOperation().outcome, CommandOutcome::NoChange);
    controller->requestShutdown(); EXPECT_EQ(controller->shutdownState(), ShutdownState::Closing); release.set();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller); return controller->shutdownState() == ShutdownState::Ready; }));
    EXPECT_DOUBLE_EQ(candidate().record->state->snapshot.cell, .5); EXPECT_EQ(candidate().session, RecoverySessionState::Closed);
}
TEST_F(Recovery, DestructorAfterPublicationWithoutReceiptPreservesSnapshot)
{
    autosave(); controller->setCell(.5); Event entered, release;
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase p) { if (p == RecoveryPhase::Published) { entered.set(); release.wait(); } };
    fault(hooks); controller->autosave();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller); return entered.isSet(); }));
    release.set(); restart();
    EXPECT_DOUBLE_EQ(candidate().record->state->snapshot.cell, .5); EXPECT_EQ(candidate().session, RecoverySessionState::Closed);
}
TEST_F(Recovery, ClosePreservesRecoveryAndConfirmedCloseDiscards)
{
    autosave(); const auto id = controller->project()->projectId();
    controller->closeProject();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller);
        return !controller->operation().active && candidate().session == RecoverySessionState::Closed; }));
    EXPECT_FALSE(controller->project()); restore(candidate()); EXPECT_EQ(controller->project()->projectId(), id);
    controller->closeProject(true);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(*controller);
        return !controller->operation().active && candidate().session == RecoverySessionState::Rejected; }));
}
TEST_F(Recovery, FailedRejectKeepsRecoverableRecord)
{
    autosave(); restart(); const auto c = candidate(); const auto before = c.record->digest;
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->commit = [](QSaveFile &) { return false; }; fault(hooks);
    controller->rejectRecovery(c); ASSERT_TRUE(finish(*controller)); EXPECT_EQ(controller->operation().status, OperationStatus::Failed);
    EXPECT_EQ(candidate().record->digest, before); EXPECT_NE(candidate().session, RecoverySessionState::Rejected);
    fault({}); restore(candidate()); EXPECT_TRUE(controller->project());
}
TEST_F(Recovery, CancelledAcceptanceKeepsPreviousProjectAndRecovery)
{
    autosave(); restart(); const auto c = candidate();
    ASSERT_EQ(controller->importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finishBeforeAutosave());
    controller->setCell(.25); const auto *before = controller->project();
    RecoveryTestAccess::acceptance(*controller, [&] { controller->cancelOperation(); });
    RecoveryRequest request{c}; request.replacementConfirmed = true;
    controller->restoreRecovery(request); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Cancelled); EXPECT_EQ(controller->project(), before);
    EXPECT_DOUBLE_EQ(controller->project()->cell(), .25); EXPECT_TRUE(controller->canUndo());
    auto found = discoverRecovery(root()); ASSERT_TRUE(found); ASSERT_EQ(found.value().size(), 1);
    EXPECT_NE(found.value()[0].session, RecoverySessionState::Rejected);
}
TEST_F(Recovery, RestoreFailureKeepsExistingProjectAndUndo)
{
    autosave(); auto c = candidate(); controller->setCell(.5); const auto state = controller->project();
    RecoveryRequest request{c}; EXPECT_EQ(controller->restoreRecovery(request).outcome, CommandOutcome::Rejected);
    request.replacementConfirmed = true; controller->restoreRecovery(request); ASSERT_TRUE(finish(*controller));
    EXPECT_EQ(controller->operation().status, OperationStatus::Failed); EXPECT_EQ(controller->project(), state); EXPECT_TRUE(controller->canUndo());
}
TEST_F(Recovery, StaleCandidateCannotRejectUpdatedRecord)
{
    autosave(); const auto stale = candidate(); controller->setCell(.5); autosave(); restart();
    EXPECT_FALSE(persistence::rejectRecovery(root(), stale)); EXPECT_DOUBLE_EQ(candidate().record->state->snapshot.cell, .5);
}
TEST_F(Recovery, AutosaveAndDiscoveryDoNotOpenCacheOrSourceOrCopyDatasets)
{
    const auto path = QString::fromStdWString(controller->project()->dataset()->cachePath().wstring());
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE); ASSERT_TRUE(QFile::remove(source));
    controller->setCell(.125); const auto started = controller->autosave(); const auto finished = finish(*controller);
    auto found = discoverRecovery(root()); CloseHandle(handle);
    ASSERT_EQ(started.outcome, CommandOutcome::Applied); ASSERT_TRUE(finished);
    EXPECT_EQ(controller->operation().status, OperationStatus::Succeeded); ASSERT_TRUE(found); ASSERT_EQ(found.value().size(), 1);
    EXPECT_DOUBLE_EQ(found.value()[0].record->state->snapshot.cell, .125);
    RecordProperty("unsavedRecordBytes", int(bytes(recordPath()).size()));
    int caches = 0; QDirIterator it(area.path(), {"*.pc2dcache"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); ++caches; } EXPECT_EQ(caches, 1);
}

class RecoveryInvalid : public Recovery, public testing::WithParamInterface<int> {};
TEST_P(RecoveryInvalid, InvalidRecordsAreReportedWithoutMutation)
{
    autosave(); const auto path = recordPath(); auto content = bytes(path); restart();
    auto parts = content.split('\n');
    if (GetParam() == 0) { content.chop(20); }
    else if (GetParam() == 1) { parts[0] = "PC2D-RECOVERY 99"; }
    else if (GetParam() == 2) { parts[4] = QByteArray("../escape.pc2dcache").toBase64(); }
    else if (GetParam() == 3) { parts[3] = QByteArray("D:/root/../escape").toBase64(); }
    else if (GetParam() == 4) { parts[4] = QByteArray("datasets/CON.pc2dcache").toBase64(); }
    else if (GetParam() == 5) { parts[5] = QByteArray("{}").toBase64(); }
    else if (GetParam() == 6) { parts[8] = "bad"; parts[6] = QByteArray("D:/project.pc2dproj").toBase64(); }
    else { parts[9] = GetParam() == 7 ? "2" : "1"; } // invalid flag / absent conflict provenance
    if (GetParam() != 0) {
        parts.removeLast(); parts.removeLast();
        content = parts.join('\n') + '\n';
        content += QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex() + '\n';
    }
    write(path, content); const auto c = candidate(); EXPECT_EQ(c.session, RecoverySessionState::Invalid); EXPECT_TRUE(c.error);
    EXPECT_EQ(bytes(path), content); EXPECT_FALSE(controller->project());
}
INSTANTIATE_TEST_SUITE_P(SchemaAndPaths, RecoveryInvalid, testing::Range(0, 9));
TEST_F(Recovery, DiscoveryResourceBudgetsAreEnforced)
{
    autosave(); RecoveryLimits limits; limits.maxRecords = 0; EXPECT_FALSE(discoverRecovery(root(), limits));
    limits = {}; limits.maxRecordBytes = 32; auto found = discoverRecovery(root(), limits);
    ASSERT_TRUE(found); ASSERT_EQ(found.value().size(), 1); EXPECT_EQ(found.value()[0].error->code, ErrorCode::ResourceExhausted);
    limits = {}; limits.maxDiscoveryBytes = 1; EXPECT_FALSE(discoverRecovery(root(), limits));
    EXPECT_FALSE(discoverRecovery(root(), {}, [](void *) noexcept { return true; }));
    auto invalid = *controller->recoveryRecord();
    auto state = std::make_shared<SavedProject>(*invalid.state); state->content.location.reset(); invalid.state = state;
    EXPECT_FALSE(RecoveryTransaction::prepare(root(), invalid, RecoveryTestAccess::lease(*controller), {}));
}

class RecoveryCrash : public testing::TestWithParam<QString> {};
TEST_P(RecoveryCrash, ProcessTerminationPreservesOnlyCompletePublishedSnapshot)
{
    QTemporaryDir area; ASSERT_TRUE(area.isValid()); const auto marker = area.path() + "/ready";
    QProcess child; child.start(QStringLiteral(RECOVERY_CRASH_PROBE_EXE), {area.path(), GetParam(), marker});
    ASSERT_TRUE(child.waitForStarted());
    ASSERT_TRUE(spinUntil([&] { return QFile::exists(marker) || child.state() == QProcess::NotRunning; }));
    ASSERT_TRUE(QFile::exists(marker)) << child.readAllStandardError().constData();
    auto live = discoverRecovery(area.path() + "/imports/recovery"); ASSERT_TRUE(live); ASSERT_EQ(live.value().size(), 1);
    EXPECT_EQ(live.value()[0].session, RecoverySessionState::Active);
    EXPECT_FALSE(persistence::restoreRecovery(area.path() + "/imports/recovery", live.value()[0], {}, {1ULL << 20, 0, 2}));
    child.kill(); ASSERT_TRUE(child.waitForFinished(10000));
    auto found = discoverRecovery(area.path() + "/imports/recovery"); ASSERT_TRUE(found); ASSERT_EQ(found.value().size(), 1);
    EXPECT_EQ(found.value()[0].session, RecoverySessionState::Interrupted);
    ProjectController controller(area.path() + "/imports");
    controller.restoreRecovery({found.value()[0]}); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), GetParam() == u"published" ? .125 : 1);
    EXPECT_EQ(controller.pointCacheAvailability()->access, CacheAccess::AvailableLimited);
    EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0);
}
INSTANTIATE_TEST_SUITE_P(Phases, RecoveryCrash, testing::Values(QStringLiteral("before"), QStringLiteral("write"),
    QStringLiteral("published"), QStringLiteral("active")));
TEST(RecoveryCrash, CrashAfterAcceptBeforeNextSnapshotRetainsSourceRecordAndDatasets)
{
    QTemporaryDir area; ASSERT_TRUE(area.isValid()); const auto marker = area.path() + "/ready";
    auto run = [&](const QString &mode) {
        QProcess child; child.start(QStringLiteral(RECOVERY_CRASH_PROBE_EXE), {area.path(), mode, marker});
        EXPECT_TRUE(child.waitForStarted());
        EXPECT_TRUE(spinUntil([&] { return QFile::exists(marker) || child.state() == QProcess::NotRunning; }));
        EXPECT_TRUE(QFile::exists(marker)); child.kill(); EXPECT_TRUE(child.waitForFinished(10000)); QFile::remove(marker);
    };
    run(QStringLiteral("published")); auto before = discoverRecovery(area.path() + "/imports/recovery");
    ASSERT_TRUE(before); ASSERT_EQ(before.value().size(), 1); const auto digest = before.value()[0].record->digest;
    run(QStringLiteral("restore")); auto after = discoverRecovery(area.path() + "/imports/recovery");
    ASSERT_TRUE(after); ASSERT_EQ(after.value().size(), 1); EXPECT_EQ(after.value()[0].record->digest, digest);
    ProjectController controller(area.path() + "/imports"); controller.restoreRecovery({after.value()[0]}); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded); EXPECT_DOUBLE_EQ(controller.project()->cell(), .125);
    EXPECT_EQ(controller.pointCacheAvailability()->access, CacheAccess::AvailableLimited);
}
TEST(RecoveryCrash, CrashBeforeFirstPublicationNeverExposesAnIncompleteRecord)
{
    QTemporaryDir area; ASSERT_TRUE(area.isValid()); const auto marker = area.path() + "/ready";
    QProcess child; child.start(QStringLiteral(RECOVERY_CRASH_PROBE_EXE), {area.path(), QStringLiteral("first"), marker});
    ASSERT_TRUE(child.waitForStarted());
    ASSERT_TRUE(spinUntil([&] { return QFile::exists(marker) || child.state() == QProcess::NotRunning; }));
    ASSERT_TRUE(QFile::exists(marker)); child.kill(); ASSERT_TRUE(child.waitForFinished(10000));
    auto found = discoverRecovery(area.path() + "/imports/recovery"); ASSERT_TRUE(found); EXPECT_TRUE(found.value().empty());
    QDirIterator files(area.path() + "/imports", {"*.pc2dcache"}, QDir::Files, QDirIterator::Subdirectories);
    ASSERT_TRUE(files.hasNext()); const auto cachePath = files.next(); EXPECT_FALSE(files.hasNext());
    auto reader = cache::CacheReader::open(cachePath, {1ULL << 20, 0, 2}); ASSERT_TRUE(reader);
    EXPECT_EQ(reader.value()->metadata().pointCount, 2); EXPECT_EQ(reader.value()->statistics().pointPages, 0);
}
TEST_F(Recovery, CrashAfterAcceptingClosedRecoveryIsDetectedAndPreservesSource)
{
    controller->setCell(.125); autosave(); restart(); ASSERT_EQ(candidate().session, RecoverySessionState::Closed);
    const auto marker = area.path() + "/ready";
    QProcess child; child.start(QStringLiteral(RECOVERY_CRASH_PROBE_EXE), {area.path(), QStringLiteral("restore"), marker});
    ASSERT_TRUE(child.waitForStarted());
    ASSERT_TRUE(spinUntil([&] { return QFile::exists(marker) || child.state() == QProcess::NotRunning; }));
    ASSERT_TRUE(QFile::exists(marker)); EXPECT_EQ(candidate().session, RecoverySessionState::Active);
    child.kill(); ASSERT_TRUE(child.waitForFinished(10000)); EXPECT_EQ(candidate().session, RecoverySessionState::Interrupted);
    restore(candidate()); EXPECT_DOUBLE_EQ(controller->project()->cell(), .125);
}
} // namespace
