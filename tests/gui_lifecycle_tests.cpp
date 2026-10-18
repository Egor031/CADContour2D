#include "ui/project_view_model.h"
#include "operation_test_support.h"
#include "project_test_access.h"

#include <QDir>
#include <QFile>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace cadcontour;
using namespace cadcontour::test;

namespace {
class GuiLifecycle : public testing::Test {
protected:
    QTemporaryDir area;
    QString storage = area.filePath("storage");
    ProjectController controller{storage};
    std::unique_ptr<ProjectViewModel> model = std::make_unique<ProjectViewModel>(controller);
    std::vector<QString> warnings;
    QQmlApplicationEngine engine;
    QPointer<QQuickWindow> window;

    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        QQmlEngine::setObjectOwnership(model.get(), QQmlEngine::CppOwnership);
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.push_back(error.toString());
        });
    }
    void TearDown() override
    {
        controller.cancelOperation();
        EXPECT_TRUE(finish(controller));
        QCoreApplication::processEvents();
        for (const auto &warning : warnings) ADD_FAILURE() << warning.toStdString();
    }
    void loadWindow(bool fileDialogEnabled = false)
    {
        engine.setInitialProperties({{"project", QVariant::fromValue(model.get())},
                                     {"fileDialogEnabled", fileDialogEnabled}});
        engine.loadFromModule("CADContour2D", "Main");
        ASSERT_EQ(engine.rootObjects().size(), 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        ASSERT_TRUE(window);
    }
    QObject *fileDialog()
    {
        auto *dialog = window->findChild<QObject *>(QStringLiteral("importFileDialog"));
        EXPECT_NE(dialog, nullptr);
        return dialog;
    }
    void useQuickFileDialog()
    {
        // Use the real production FileDialog/open handler, without an OS dialog in CTest.
        QQmlExpression options(qmlContext(fileDialog()), fileDialog(),
            QStringLiteral("options = NativeDialogs.FileDialog.DontUseNativeDialog"));
        options.evaluate();
        ASSERT_FALSE(options.hasError()) << options.error().toString().toStdString();
    }
    QString source()
    {
        const auto path = area.filePath("source.xyz");
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write("1 2 3\n4 5 0\n"), 12);
        return path;
    }
    auto startWaitingImport()
    {
        model->newProject();
        model->selectImportFile(model->selectionToken(), QUrl::fromLocalFile(source()));
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        EXPECT_TRUE(context);
        if (context) {
            std::unique_lock lock(context->mutex);
            EXPECT_TRUE(context->decisionChanged.wait_for(lock, std::chrono::seconds(10), [&] {
                return context->waitingForZ;
            }));
        }
        return context;
    }
    void expectCancelledImport()
    {
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
        EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Cancelled);
        EXPECT_FALSE(controller.operation().failure);
        EXPECT_FALSE(controller.pendingNonZeroZ());
        EXPECT_EQ(controller.project(), nullptr);
        EXPECT_FALSE(OperationTestAccess::runner(controller).busy());
        EXPECT_TRUE(QDir(storage).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }
};
}

TEST_F(GuiLifecycle, R1DeletionInsideZNotificationCancelsAndWakesWorker)
{
    const auto context = startWaitingImport();
    ASSERT_TRUE(context);
    QPointer<ProjectViewModel> lifetime(model.get());
    QObject observer;
    int delivered = 0;
    bool woke = false;
    QObject::connect(model.get(), &ProjectViewModel::nonZeroZChanged, &observer, [&] {
        ++delivered;
        model.reset();
        // The worker must reach Ready without any subsequent GUI events/poll/shutdown/drain.
        woke = waitContext(context, false);
    });
    // The normal OperationRunner timer delivers the notification after the observed Z-wait.
    ASSERT_TRUE(spinUntil([&] { return lifetime.isNull(); }));
    EXPECT_EQ(delivered, 1);
    EXPECT_TRUE(woke);
    expectCancelledImport();
}

class ReentrantShutdown : public GuiLifecycle, public testing::WithParamInterface<bool> {};
TEST_P(ReentrantShutdown, R2RevokedSelectionDoesNotRequestFileDialog)
{
    if (GetParam()) {
        ProjectControllerTestAccess::newProject(controller);
        controller.setCell(2);
        controller.undo();
    }
    const auto before = controller.importContext();
    ASSERT_TRUE(before);
    const bool hadRedo = controller.canRedo();
    QObject observer;
    QString createdToken;
    int requested = 0;
    QObject::connect(model.get(), &ProjectViewModel::fileSelectionRequested, &observer, [&] { ++requested; });
    QObject::connect(model.get(), &ProjectViewModel::importUiChanged, &observer, [&] {
        if (model->selectingFile()) {
            createdToken = model->selectionToken();
            controller.requestShutdown();
        }
    });
    model->newProject();
    EXPECT_FALSE(createdToken.isEmpty());
    EXPECT_EQ(requested, 0);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_FALSE(model->selectingFile());
    EXPECT_TRUE(model->selectionToken().isEmpty());
    EXPECT_EQ(model->hasProject(), GetParam());
    if (before->project) EXPECT_TRUE(controller.project()->accepts(*before->project));
    EXPECT_EQ(controller.canRedo(), hadRedo);
    EXPECT_EQ(controller.operation().id, 0);
}
INSTANTIATE_TEST_SUITE_P(Contexts, ReentrantShutdown, testing::Values(false, true));

TEST_F(GuiLifecycle, R2ShutdownDuringSelectionDoesNotOpenProductionFileDialog)
{
    loadWindow(true);
    useQuickFileDialog();
    QObject observer;
    QString createdToken;
    QObject::connect(model.get(), &ProjectViewModel::importUiChanged, &observer, [&] {
        if (model->selectingFile()) {
            createdToken = model->selectionToken();
            controller.requestShutdown();
        }
    });
    model->newProject();
    EXPECT_FALSE(createdToken.isEmpty());
    EXPECT_TRUE(model->closing());
    EXPECT_FALSE(model->selectingFile());
    EXPECT_TRUE(model->selectionToken().isEmpty());
    EXPECT_FALSE(fileDialog()->property("visible").toBool());
    EXPECT_TRUE(fileDialog()->property("requestToken").toString().isEmpty());
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(controller.operation().id, 0);
    QMetaObject::invokeMethod(fileDialog(), "close");
}

TEST_F(GuiLifecycle, R3DeletedWindowBeforeDeferredStartupDoesNotAccessQmlContext)
{
    loadWindow();
    ASSERT_FALSE(model->selectingFile());
    delete window.data();
    EXPECT_TRUE(window.isNull());
    QCoreApplication::processEvents();
    EXPECT_FALSE(model->selectingFile());
    EXPECT_EQ(controller.operation().id, 0);
    EXPECT_EQ(controller.project(), nullptr);
}

TEST_F(GuiLifecycle, R3DeletedWindowBeforeDeferredCloseDoesNotAccessQmlContext)
{
    loadWindow();
    QCoreApplication::processEvents(); // drain startup while the window is still alive
    model->cancelFileSelection(model->selectionToken());
    controller.requestShutdown();
    EXPECT_TRUE(model->shutdownReady());
    delete window.data();
    QCoreApplication::processEvents();
    EXPECT_TRUE(window.isNull());
    EXPECT_EQ(controller.operation().id, 0);
    EXPECT_EQ(controller.project(), nullptr);
}

using ModelSignal = void (ProjectViewModel::*)();
class StateNotificationDeletion : public GuiLifecycle, public testing::WithParamInterface<ModelSignal> {};
TEST_P(StateNotificationDeletion, R1DeletionAtStateNotificationBoundaryStopsContinuation)
{
    ProjectControllerTestAccess::newProject(controller);
    model->applyCell(QStringLiteral("invalid"));
    QPointer<ProjectViewModel> lifetime(model.get());
    QObject observer;
    QObject::connect(model.get(), GetParam(), &observer, [&] { model.reset(); });
    EXPECT_EQ(controller.setCell(2).outcome, CommandOutcome::Applied);
    EXPECT_TRUE(lifetime.isNull());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2);
    EXPECT_TRUE(controller.canUndo());
    EXPECT_EQ(controller.operation().id, 0);
}
INSTANTIATE_TEST_SUITE_P(Signals, StateNotificationDeletion, testing::Values(
    &ProjectViewModel::errorMessageChanged, &ProjectViewModel::stateChanged,
    &ProjectViewModel::cellInputSyncRequested, &ProjectViewModel::operationChanged));

TEST_F(GuiLifecycle, R1DeletionInsideSelectionNotificationStopsRequest)
{
    QPointer<ProjectViewModel> lifetime(model.get());
    QObject observer;
    QObject::connect(model.get(), &ProjectViewModel::importUiChanged, &observer, [&] { model.reset(); });
    model->newProject();
    EXPECT_TRUE(lifetime.isNull());
    EXPECT_EQ(controller.operation().id, 0);
    EXPECT_EQ(controller.project(), nullptr);
}

TEST_F(GuiLifecycle, R1DeletionWhileRevokingSelectionStopsNotifications)
{
    model->newProject();
    QPointer<ProjectViewModel> lifetime(model.get());
    QObject observer;
    QObject::connect(model.get(), &ProjectViewModel::importUiChanged, &observer, [&] { model.reset(); });
    controller.requestShutdown();
    EXPECT_TRUE(lifetime.isNull());
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(controller.operation().id, 0);
}

TEST_F(GuiLifecycle, R1DeletionBeforeLaunchDoesNotStartImport)
{
    model->newProject();
    const auto token = model->selectionToken();
    QPointer<ProjectViewModel> lifetime(model.get());
    QObject observer;
    QObject::connect(model.get(), &ProjectViewModel::importUiChanged, &observer, [&] { model.reset(); });
    model->selectImportFile(token, QUrl::fromLocalFile(source()));
    EXPECT_TRUE(lifetime.isNull());
    EXPECT_EQ(controller.operation().id, 0);
    EXPECT_EQ(controller.project(), nullptr);
}

TEST_F(GuiLifecycle, R1DeletionWhileCanonicalizingNoOpStopsInputSync)
{
    ProjectControllerTestAccess::newProject(controller);
    model->applyCell(QStringLiteral("invalid"));
    QPointer<ProjectViewModel> lifetime(model.get());
    QObject observer;
    QObject::connect(model.get(), &ProjectViewModel::errorMessageChanged, &observer, [&] { model.reset(); });
    model->applyCell(QStringLiteral("1"));
    EXPECT_TRUE(lifetime.isNull());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
    EXPECT_FALSE(controller.canUndo());
}

TEST_F(GuiLifecycle, R2SupersededSelectionOnlyRequestsCurrentToken)
{
    QObject observer;
    bool superseded = false;
    QString oldToken;
    std::vector<QString> requested;
    QObject::connect(model.get(), &ProjectViewModel::importUiChanged, &observer, [&] {
        if (!superseded && model->selectingFile()) {
            superseded = true;
            oldToken = model->selectionToken();
            model->cancelFileSelection(oldToken);
            model->newProject();
        }
    });
    QObject::connect(model.get(), &ProjectViewModel::fileSelectionRequested, &observer, [&] {
        requested.push_back(model->selectionToken());
    });
    model->newProject();
    ASSERT_EQ(requested.size(), 1);
    EXPECT_NE(requested.front(), oldToken);
    EXPECT_EQ(requested.front(), model->selectionToken());
    EXPECT_EQ(controller.operation().id, 0);
}

class CommandNotificationDeletion : public GuiLifecycle, public testing::WithParamInterface<int> {};
TEST_P(CommandNotificationDeletion, R1ControllerCommandCanDestroyAdapterBeforeReturning)
{
    std::shared_ptr<detail::OperationContext> context;
    if (GetParam() == 3) {
        context = startWaitingImport();
        ASSERT_TRUE(context);
    } else {
        ProjectControllerTestAccess::newProject(controller);
        controller.setCell(2);
        if (GetParam() == 2) controller.undo();
    }
    QObject observer;
    QPointer<ProjectViewModel> lifetime(model.get());
    const ModelSignal signal = GetParam() == 3 ? &ProjectViewModel::operationChanged
        : GetParam() == 4 ? &ProjectViewModel::shutdownChanged : &ProjectViewModel::stateChanged;
    QObject::connect(model.get(), signal, &observer, [&] { model.reset(); });
    switch (GetParam()) {
    case 0: model->applyCell(QStringLiteral("4")); break;
    case 1: model->undo(); break;
    case 2: model->redo(); break;
    case 3: model->cancelOperation(); break;
    case 4: EXPECT_TRUE(model->requestClose()); break;
    }
    EXPECT_TRUE(lifetime.isNull());
    if (context) {
        ASSERT_TRUE(waitContext(context, false));
        expectCancelledImport();
    } else {
        EXPECT_DOUBLE_EQ(controller.project()->cell(), GetParam() == 0 ? 4 : GetParam() == 1 ? 1 : 2);
        EXPECT_EQ(controller.operation().id, 0);
    }
}
INSTANTIATE_TEST_SUITE_P(Commands, CommandNotificationDeletion, testing::Values(0, 1, 2, 3, 4));

TEST_F(GuiLifecycle, R2StalePresentationNotificationDoesNotOpenFileDialog)
{
    ProjectControllerTestAccess::newProject(controller);
    loadWindow(true);
    useQuickFileDialog();
    EXPECT_FALSE(model->selectingFile());
    emit model->fileSelectionRequested();
    EXPECT_FALSE(fileDialog()->property("visible").toBool());
    QMetaObject::invokeMethod(fileDialog(), "close");
    controller.requestShutdown();
    emit model->fileSelectionRequested();
    EXPECT_FALSE(fileDialog()->property("visible").toBool());
    QMetaObject::invokeMethod(fileDialog(), "close");
    EXPECT_EQ(controller.operation().id, 0);
}
