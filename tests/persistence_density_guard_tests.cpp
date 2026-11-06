#include "operation_test_support.h"
#include "recovery_test_access.h"
#include "ui/project_view_model.h"

#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace cadcontour;
using namespace cadcontour::test;

namespace {
class DensityPersistence : public testing::Test {
protected:
    QTemporaryDir area;
    ProjectController controller{area.filePath("storage")};
    QString source = area.filePath("source.xyz");
    QString target = area.filePath("saved/project.pc2dproj");

    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("0 0 0\n1 1 0\n"), 12); file.close();
        ASSERT_EQ(controller.importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}}).outcome,
                  CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        RecoveryTestAccess::timers(controller);
        ASSERT_TRUE(finishBackgroundAutosave(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    }
    QByteArray bytes(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
    void save()
    {
        ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        RecoveryTestAccess::timers(controller);
        ASSERT_TRUE(finishBackgroundAutosave(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    }
    void build()
    {
        ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        ASSERT_EQ(controller.project()->densityVersions().size(), 1);
    }
    QString densityPath() const
    {
        return QString::fromStdWString(controller.project()->densityVersions().front()
            ->descriptor().dataset.location->path.wstring());
    }
    void expectUnsupported(const CommandResult &result)
    {
        ASSERT_EQ(result.outcome, CommandOutcome::Rejected);
        ASSERT_TRUE(result.failure);
        EXPECT_EQ(result.failure->category, CommandError::UnsafeState);
        EXPECT_EQ(result.failure->detail, ErrorDetail::PersistenceVersionsUnsupported);
    }
};
}

TEST_F(DensityPersistence, AllControllerSavePathsRefuseBeforeWorkerAndPreservePublishedBytes)
{
    save();
    ASSERT_EQ(controller.setCell(.5).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    const auto baseline = controller.savedProject();
    const auto recovery = controller.recoveryRecord();
    ASSERT_TRUE(recovery);
    const auto recoveryPath = RecoveryTestAccess::root(controller) + u'/'
        + persistence::recoveryFileName(recovery->sessionId);
    const auto manifestBytes = bytes(target), recoveryBytes = bytes(recoveryPath);
    ASSERT_FALSE(manifestBytes.isEmpty()); ASSERT_FALSE(recoveryBytes.isEmpty());
    build();
    const auto output = densityPath(); const auto densityBytes = bytes(output);
    ASSERT_FALSE(densityBytes.isEmpty());
    const auto operation = controller.operation().id;
    const auto stamp = controller.project()->operationStamp();
    const auto versions = controller.project()->densityVersions();
    for (int attempt = 0; attempt != 3; ++attempt) {
        expectUnsupported(controller.saveProject());
        expectUnsupported(controller.saveProject({area.filePath("other/name.pc2dproj")}));
        expectUnsupported(controller.saveProjectAs({area.filePath("other/name.pc2dproj")}));
        expectUnsupported(controller.autosave());
    }
    EXPECT_EQ(controller.operation().id, operation); EXPECT_FALSE(controller.operation().active);
    EXPECT_EQ(controller.project()->operationStamp(), stamp);
    EXPECT_EQ(controller.project()->densityVersions(), versions);
    EXPECT_EQ(controller.savedProject(), baseline); EXPECT_EQ(controller.recoveryRecord(), recovery);
    EXPECT_TRUE(controller.isDirty());
    EXPECT_EQ(bytes(target), manifestBytes); EXPECT_EQ(bytes(recoveryPath), recoveryBytes);
    EXPECT_EQ(bytes(output), densityBytes);
    EXPECT_FALSE(QFile::exists(area.filePath("other/name.pc2dproj")));
}

TEST_F(DensityPersistence, DirectSnapshotCaptureAndDirtyStayGuardedAfterUndoToNull)
{
    save(); build();
    const auto output = densityPath();
    const auto original = bytes(output);
    ProjectViewModel model(controller);
    EXPECT_TRUE(model.densityMapStatusText().contains(QStringLiteral("Активная карта")));
    auto capture = persistence::captureSnapshot(*controller.project(), QUuid::createUuid(), QUuid::createUuid());
    ASSERT_FALSE(capture);
    EXPECT_EQ(capture.error().code, persistence::ErrorCode::UnrepresentableState);
    EXPECT_EQ(capture.error().field, QStringLiteral("state.densityVersions"));
    EXPECT_FALSE(persistence::isV1Representable(*controller.project()));
    EXPECT_FALSE(persistence::matchesSavedContent(*controller.project(), *controller.savedProject()));
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_FALSE(controller.project()->activeDensityVersion());
    EXPECT_TRUE(model.densityMapStatusText().contains(QStringLiteral("не выбрана")));
    EXPECT_EQ(controller.project()->densityVersions().size(), 1);
    EXPECT_TRUE(controller.canRedo()); EXPECT_TRUE(controller.isDirty());
    capture = persistence::captureSnapshot(*controller.project(), QUuid::createUuid(), QUuid::createUuid());
    ASSERT_FALSE(capture); EXPECT_EQ(capture.error().code, persistence::ErrorCode::UnrepresentableState);
    expectUnsupported(controller.saveProject()); expectUnsupported(controller.autosave());
    EXPECT_TRUE(controller.canRedo()); EXPECT_EQ(bytes(output), original);
}

TEST_F(DensityPersistence, ExistingV1CaptureAndSaveRemainAvailableWithoutVersions)
{
    EXPECT_TRUE(persistence::isV1Representable(*controller.project()));
    ASSERT_TRUE(persistence::checkV1Representable(*controller.project()));
    ASSERT_TRUE(persistence::captureSnapshot(*controller.project(), QUuid::createUuid(), QUuid::createUuid()));
    save(); EXPECT_FALSE(controller.isDirty());
    ASSERT_EQ(controller.setCell(.125).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_FALSE(controller.isDirty());
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_TRUE(controller.isDirty());
}

class DensitySaveBeforeReplacement : public DensityPersistence, public testing::WithParamInterface<int> {};
TEST_P(DensitySaveBeforeReplacement, RefusedSaveStopsCloseExitOpenAndImport)
{
    save(); build();
    ProjectViewModel model(controller);
    EXPECT_FALSE(model.canSave());
    EXPECT_FALSE(model.saveStatusText().isEmpty());
    EXPECT_TRUE(model.cacheAvailable());
    const auto stamp = controller.project()->operationStamp();
    const auto operation = controller.operation().id;
    switch (GetParam()) {
    case 0: model.closeProject(); break;
    case 1: EXPECT_FALSE(model.requestClose()); break;
    case 2:
        model.open();
        ASSERT_EQ(model.workflowPrompt(), QStringLiteral("open"));
        model.selectProjectFile(model.workflowToken(), QUrl::fromLocalFile(target));
        break;
    case 3:
        model.newProject();
        ASSERT_TRUE(model.selectingFile());
        model.selectImportFile(model.selectionToken(), QUrl::fromLocalFile(source));
        ASSERT_TRUE(model.replacementPending());
        model.confirmReplacement(model.selectionToken(), true); break;
    }
    ASSERT_EQ(model.workflowPrompt(), QStringLiteral("dirty"));
    model.answerWorkflow(model.workflowToken(), QStringLiteral("save"));
    EXPECT_FALSE(model.workflowPending()); EXPECT_TRUE(model.workflowPrompt().isEmpty());
    EXPECT_TRUE(model.fileMessage().contains(QStringLiteral("текущем формате")));
    EXPECT_TRUE(controller.project()); EXPECT_EQ(controller.project()->operationStamp(), stamp);
    EXPECT_EQ(controller.operation().id, operation); EXPECT_FALSE(controller.operation().active);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Open); EXPECT_TRUE(controller.isDirty());
}
INSTANTIATE_TEST_SUITE_P(CloseExitOpenImport, DensitySaveBeforeReplacement, testing::Range(0, 4));

TEST_F(DensityPersistence, CloseCancelAndExplicitDiscardRemainAvailableAndRetainAcceptedFile)
{
    build(); ProjectViewModel model(controller);
    const auto output = densityPath(); const auto original = bytes(output);
    model.closeProject(); ASSERT_EQ(model.workflowPrompt(), QStringLiteral("dirty"));
    model.answerWorkflow(model.workflowToken(), QStringLiteral("cancel"));
    EXPECT_TRUE(controller.project()); EXPECT_FALSE(model.workflowPending());
    model.closeProject(); ASSERT_EQ(model.workflowPrompt(), QStringLiteral("dirty"));
    model.answerWorkflow(model.workflowToken(), QStringLiteral("discard"));
    EXPECT_FALSE(controller.project()); EXPECT_FALSE(model.workflowPending());
    EXPECT_EQ(bytes(output), original);
}
