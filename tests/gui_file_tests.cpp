#include "ui/project_view_model.h"
#include "ui/application_shutdown.h"
#include "operation_test_support.h"
#include "recovery_test_access.h"
#include "save_test_access.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QTemporaryDir>
#include <QPointer>
#include <gtest/gtest.h>

#include <tuple>

using namespace cadcontour;
using namespace cadcontour::test;

namespace {
class FileGui : public testing::Test {
protected:
    QTemporaryDir area;
    ProjectController controller{area.filePath("storage")};
    ProjectViewModel model{controller};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    std::vector<QString> warnings;

    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        const auto source = area.filePath("source.xyz");
        QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("1 2 0\n3 4 0\n"), 12); file.close();
        ASSERT_EQ(controller.importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}}).outcome, CommandOutcome::Applied);
        settle();
        QQmlEngine::setObjectOwnership(&model, QQmlEngine::CppOwnership);
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.push_back(error.toString());
        });
        engine.setInitialProperties({{"project", QVariant::fromValue(&model)}, {"fileDialogEnabled", false}});
        engine.loadFromModule("CADContour2D", "Main");
        ASSERT_EQ(engine.rootObjects().size(), 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front()); ASSERT_TRUE(window);
        QCoreApplication::processEvents();
    }
    void TearDown() override
    {
        controller.cancelOperation(); settle();
        for (const auto &warning : warnings) ADD_FAILURE() << warning.toStdString();
    }
    void settle()
    {
        ASSERT_TRUE(spinUntil([&] {
            OperationTestAccess::poll(controller);
            return RecoveryTestAccess::settled(controller)
                && (!model.workflowPending() || !model.workflowPrompt().isEmpty());
        }));
        RecoveryTestAccess::timers(controller);
        QCoreApplication::processEvents();
    }
    QObject *control(const char *name)
    {
        auto *object = window->findChild<QObject *>(QString::fromLatin1(name));
        EXPECT_NE(object, nullptr) << name; return object;
    }
    void invoke(const char *name, const char *signal)
    { ASSERT_TRUE(QMetaObject::invokeMethod(control(name), signal, Qt::DirectConnection)); }
    void click(const char *name) { invoke(name, "clicked"); }
    void action(const char *name) { invoke(name, "triggered"); }
    void draft(const QString &text) { ASSERT_TRUE(control("cellInput")->setProperty("text", text)); }
    void answer(const char *value) { model.answerWorkflow(model.workflowToken(), QString::fromLatin1(value)); }
    void choose(const QString &path)
    {
        ASSERT_TRUE(control("projectFileDialog")->setProperty("selectedFile", QUrl::fromLocalFile(path)));
        invoke("projectFileDialog", "accepted");
    }
    QString path(const char *name = "first") { return area.filePath(QString::fromLatin1(name) + "/project.pc2dproj"); }
    void firstSave()
    {
        action("saveAction"); ASSERT_EQ(model.workflowPrompt(), "save"); choose(path()); settle();
        ASSERT_TRUE(controller.savedProject()); ASSERT_FALSE(model.dirty());
    }
    void modified()
    { draft(QString::number(model.cell() + 1)); click("applyButton"); ASSERT_TRUE(model.dirty()); ASSERT_TRUE(controller.canUndo()); }
    QString invalidManifest()
    {
        const auto name = area.filePath("invalid.pc2dproj");
        QFile file(name); EXPECT_TRUE(file.open(QIODevice::WriteOnly)); file.write("{"); return name;
    }
};
}

TEST_F(FileGui, FirstRepeatedSaveAndSaveAsUseProductionDialogs)
{
    firstSave(); EXPECT_TRUE(window->title().contains("project")); EXPECT_FALSE(window->title().contains('*'));
    const auto firstSnapshot = controller.savedProject()->snapshot.snapshotId;
    modified(); action("saveAction"); EXPECT_TRUE(model.workflowPrompt().isEmpty()); settle();
    EXPECT_NE(controller.savedProject()->snapshot.snapshotId, firstSnapshot); EXPECT_FALSE(model.dirty());
    action("saveAsAction"); EXPECT_EQ(model.workflowPrompt(), "save"); choose(path("second")); settle();
    EXPECT_EQ(model.projectPath(), path("second")); EXPECT_TRUE(QFile::exists(path()));
    EXPECT_TRUE(controller.canUndo()); EXPECT_FALSE(model.dirty());
}

class FileMessageImport : public FileGui, public testing::WithParamInterface<bool> {};
TEST_P(FileMessageImport, L1ImportAfterSaveShowsOnlyTheNewUnsavedProject)
{
    action("saveAction"); ASSERT_EQ(model.workflowPrompt(), "save");
    const auto saveToken = model.workflowToken(); choose(path()); settle();
    ASSERT_FALSE(model.dirty());
    ASSERT_EQ(control("fileMessageLabel")->property("text").toString(), QStringLiteral("Проект сохранён."));
    const auto identity = model.runtimeIdentity();
    const auto incoming = area.filePath("next.xyz");
    QFile file(incoming); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("8 9 0\n"), 6); file.close();
    if (GetParam()) {
        click("newProjectButton");
        control("importFileDialog")->setProperty("selectedFile", QUrl::fromLocalFile(incoming));
        invoke("importFileDialog", "accepted"); click("replaceConfirmButton");
    } else {
        ASSERT_EQ(controller.importAscXyz({incoming, {8ULL << 20, 64ULL << 20, 8}, true}).outcome, CommandOutcome::Applied);
    }
    settle();
    ASSERT_TRUE(model.hasProject()); EXPECT_NE(model.runtimeIdentity(), identity);
    EXPECT_EQ(model.sourceName(), "next.xyz"); EXPECT_EQ(model.pointCountText(), "1");
    EXPECT_EQ(model.projectName(), QStringLiteral("Без имени")); EXPECT_TRUE(model.projectPath().isEmpty());
    EXPECT_TRUE(model.dirty()); EXPECT_TRUE(model.cacheAvailable()); EXPECT_DOUBLE_EQ(model.cell(), 1);
    EXPECT_FALSE(controller.canUndo()); EXPECT_FALSE(controller.canRedo());
    EXPECT_TRUE(window->title().contains(QStringLiteral("Без имени *")));
    EXPECT_TRUE(control("projectPathLabel")->property("text").toString().contains(QStringLiteral("Расположение ещё не выбрано")));
    EXPECT_TRUE(control("fileMessageLabel")->property("text").toString().isEmpty());
    EXPECT_FALSE(control("fileMessageLabel")->property("visible").toBool());
    const auto operationId = controller.operation().id;
    model.selectProjectFile(saveToken, QUrl::fromLocalFile(path("stale")));
    model.answerWorkflow(saveToken, "save");
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "operationChanged", Qt::QueuedConnection));
    QCoreApplication::processEvents();
    EXPECT_EQ(controller.operation().id, operationId); EXPECT_FALSE(QFile::exists(path("stale")));
    EXPECT_TRUE(model.dirty()); EXPECT_TRUE(model.projectPath().isEmpty());
    EXPECT_TRUE(control("fileMessageLabel")->property("text").toString().isEmpty());
}
INSTANTIATE_TEST_SUITE_P(ControllerAndQml, FileMessageImport, testing::Bool());

TEST_F(FileGui, L1CloseRetiresSaveMessageAndLateNotifications)
{
    firstSave();
    ASSERT_EQ(model.fileMessage(), QStringLiteral("Проект сохранён."));
    const auto saveId = controller.operation().id;
    ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_FALSE(model.hasProject()); EXPECT_TRUE(model.projectPath().isEmpty());
    EXPECT_TRUE(control("fileMessageLabel")->property("text").toString().isEmpty());
    // Delayed notification hints read current controller state, not a captured result.
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "operationChanged", Qt::QueuedConnection));
    QCoreApplication::processEvents(); settle();
    EXPECT_GE(controller.operation().id, saveId);
    EXPECT_TRUE(control("fileMessageLabel")->property("text").toString().isEmpty());
    EXPECT_TRUE(window->isVisible());
}

TEST_F(FileGui, L1OpenKeepsItsOwnResultAfterSessionReplacement)
{
    firstSave(); const auto identity = model.runtimeIdentity();
    action("openAction"); choose(path()); settle();
    EXPECT_NE(model.runtimeIdentity(), identity); EXPECT_EQ(model.projectPath(), path());
    EXPECT_FALSE(model.dirty()); EXPECT_TRUE(model.cacheAvailable());
    const auto message = QStringLiteral("Проект открыт.");
    EXPECT_EQ(control("fileMessageLabel")->property("text").toString(), message);
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "stateChanged", Qt::QueuedConnection));
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "operationChanged", Qt::QueuedConnection));
    QCoreApplication::processEvents();
    EXPECT_EQ(control("fileMessageLabel")->property("text").toString(), message);
    action("closeProjectAction"); settle();
    EXPECT_FALSE(model.hasProject()); EXPECT_TRUE(model.fileMessage().isEmpty());
}

TEST_F(FileGui, L1ClosedSessionIgnoresLateOpenCancellation)
{
    firstSave(); ControlledWork barrier;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false,
        [&] { barrier.entered.set(); barrier.complete.wait(); });
    action("openAction"); choose(path()); barrier.entered.wait();
    const auto openId = controller.operation().id;
    ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_FALSE(model.hasProject());
    QObject observer; bool terminalObserved = false;
    QObject::connect(&controller, &ProjectController::operationChanged, &observer, [&] {
        if (controller.operation().id == openId && !controller.operation().active) {
            terminalObserved = true;
            EXPECT_NE(controller.operation().status, OperationStatus::Succeeded);
            EXPECT_TRUE(control("fileMessageLabel")->property("text").toString().isEmpty());
        }
    });
    barrier.complete.set(); settle();
    EXPECT_TRUE(terminalObserved); EXPECT_FALSE(model.hasProject());
    EXPECT_TRUE(control("fileMessageLabel")->property("text").toString().isEmpty());
}

class FileMessageDiagnostic : public FileGui, public testing::WithParamInterface<int> {};
TEST_P(FileMessageDiagnostic, L1CurrentSaveDiagnosticSurvivesEditsAndNotifications)
{
    firstSave(); modified(); const auto identity = model.runtimeIdentity(); ControlledWork barrier;
    if (GetParam() == 0)
        SaveTestAccess::configure(controller, {}, [](QIODevice &, const char *, qint64) -> qint64 { return -1; });
    else if (GetParam() == 1)
        SaveTestAccess::configure(controller, [&](persistence::SavePhase phase) {
            if (phase == persistence::SavePhase::ManifestPrepare) { barrier.entered.set(); barrier.complete.wait(); }
        });
    else SaveTestAccess::configure(controller, {}, {}, [](QSaveFile &) { return false; });
    action("saveAction");
    if (GetParam() == 1) { barrier.entered.wait(); click("stopOperationButton"); barrier.complete.set(); }
    settle();
    EXPECT_EQ(model.runtimeIdentity(), identity); EXPECT_TRUE(model.dirty());
    EXPECT_EQ(controller.operation().status, GetParam() == 1 ? OperationStatus::Cancelled : OperationStatus::Failed);
    EXPECT_EQ(bool(controller.unconfirmedSave()), GetParam() == 2);
    const auto message = control("fileMessageLabel")->property("text").toString();
    EXPECT_TRUE(message.contains(GetParam() == 1 ? QStringLiteral("отменена")
        : GetParam() == 2 ? QStringLiteral("неопределён") : QStringLiteral("Ошибка")));
    EXPECT_FALSE(message.contains(QStringLiteral("Проект сохранён.")));
    ASSERT_EQ(controller.setCell(3).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(QMetaObject::invokeMethod(&controller, "operationChanged", Qt::QueuedConnection));
    QCoreApplication::processEvents();
    EXPECT_EQ(control("fileMessageLabel")->property("text").toString(), message);
    EXPECT_TRUE(control("fileMessageLabel")->property("visible").toBool());
}
INSTANTIATE_TEST_SUITE_P(FailureCancellationUncertain, FileMessageDiagnostic, testing::Range(0, 3));

TEST_F(FileGui, CancelPathAndConflictPreserveProjectHistoryAndDraft)
{
    modified(); const auto stamp = controller.project()->operationStamp(); const auto id = controller.operation().id;
    action("saveAction"); const auto token = model.workflowToken(); invoke("projectFileDialog", "rejected");
    model.selectProjectFile(token, QUrl::fromLocalFile(path()));
    EXPECT_EQ(controller.operation().id, id); EXPECT_TRUE(controller.project()->accepts(stamp)); EXPECT_TRUE(model.dirty());
    firstSave(); modified();
    const auto baseline = controller.savedProject()->snapshot.snapshotId;
    action("saveAsAction"); choose(QFileInfo(path()).dir().filePath("another.pc2dproj")); settle();
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, baseline);
    EXPECT_TRUE(model.dirty()); EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(model.fileMessage().contains("Save:"));
}

TEST_F(FileGui, OpenWithoutSourceRestoresCellCleanStateAndEmptyHistory)
{
    modified(); firstSave(); ASSERT_TRUE(QFile::remove(area.filePath("source.xyz")));
    const auto identity = controller.project()->runtimeIdentity(); controller.setCell(3);
    action("openAction"); choose(path()); EXPECT_EQ(model.workflowPrompt(), "dirty"); answer("discard"); settle();
    EXPECT_NE(controller.project()->runtimeIdentity(), identity); EXPECT_DOUBLE_EQ(model.cell(), 2);
    EXPECT_EQ(control("cellInput")->property("text").toString(), "2");
    EXPECT_FALSE(model.dirty()); EXPECT_FALSE(controller.canUndo()); EXPECT_FALSE(controller.canRedo());
    EXPECT_TRUE(model.cacheAvailable());
}

TEST_F(FileGui, OpenFailureAndRejectedReplacementRetainSessionAndHistory)
{
    firstSave(); modified(); const auto stamp = controller.project()->operationStamp();
    action("openAction"); choose(path()); click("decisionCancelButton");
    EXPECT_TRUE(controller.project()->accepts(stamp)); EXPECT_TRUE(controller.canUndo());
    action("openAction"); choose(invalidManifest()); answer("discard"); settle();
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_TRUE(controller.project()->accepts(stamp)); EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(model.dirty());
}

TEST_F(FileGui, DraftApplyDiscardCancelAndInvalidInputAreSeparateFromDirty)
{
    firstSave(); draft("1e-"); action("saveAction"); EXPECT_EQ(model.workflowPrompt(), "draft");
    click("decisionAcceptButton"); EXPECT_TRUE(model.hasDraft()); EXPECT_FALSE(model.errorMessage().isEmpty());
    EXPECT_FALSE(model.workflowPending()); EXPECT_FALSE(model.dirty());
    draft("0,5"); action("saveAction"); invoke("decisionDialog", "close");
    EXPECT_EQ(control("cellInput")->property("text").toString(), "0,5"); EXPECT_FALSE(model.workflowPending());
    action("saveAction"); click("decisionAcceptButton"); settle(); EXPECT_DOUBLE_EQ(model.cell(), .5); EXPECT_FALSE(model.dirty());
    draft("bad"); action("saveAction"); click("decisionDiscardButton"); settle();
    EXPECT_FALSE(model.hasDraft()); EXPECT_TRUE(model.errorMessage().isEmpty());
    click("undoButton"); EXPECT_TRUE(model.dirty()); click("redoButton"); EXPECT_FALSE(model.dirty());
}

TEST_F(FileGui, SaveBeforeOpenUsesCurrentSaveDestinationAndContinuesOnlyAfterReceipt)
{
    firstSave(); modified(); const auto identity = controller.project()->runtimeIdentity();
    action("openAction"); choose(path()); answer("save");
    EXPECT_EQ(controller.project()->runtimeIdentity(), identity);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return !model.workflowPending(); })); settle();
    EXPECT_NE(controller.project()->runtimeIdentity(), identity); EXPECT_DOUBLE_EQ(model.cell(), 2); EXPECT_FALSE(model.dirty());
}

TEST_F(FileGui, UnsavedSaveBeforeOpenRequestsSeparateSavePath)
{
    // A missing incoming manifest must never become the destination of the old session.
    const auto incoming = invalidManifest();
    action("openAction"); choose(incoming); answer("save");
    EXPECT_EQ(model.workflowPrompt(), "save"); choose(path());
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return !model.workflowPending(); })); settle();
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_TRUE(QFile::exists(path())); QFile file(incoming); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), "{"); EXPECT_TRUE(controller.project());
}

TEST_F(FileGui, PartialOpenAndRecheckAreBoundedAndRespectControllerGuards)
{
    firstSave(); const auto cachePath = QString::fromStdWString(controller.project()->dataset()->cachePath().wstring());
    const auto hidden = cachePath + ".hidden"; ASSERT_TRUE(QFile::rename(cachePath, hidden));
    action("openAction"); choose(path()); settle();
    EXPECT_FALSE(model.cacheAvailable()); EXPECT_FALSE(control("applyButton")->property("enabled").toBool());
    EXPECT_FALSE(model.canSave()); EXPECT_TRUE(model.cacheStatusText().contains(QStringLiteral("отсутствует")));
    EXPECT_FALSE(controller.pointCacheAvailability()->reads.pointPages);
    const auto stamp = controller.project()->operationStamp();
    ASSERT_TRUE(QFile::rename(hidden, cachePath)); click("recheckCacheButton"); settle();
    EXPECT_TRUE(model.cacheAvailable()); EXPECT_TRUE(controller.project()->accepts(stamp)); EXPECT_FALSE(model.dirty());
    EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0);
    EXPECT_LE(controller.pointCacheAvailability()->reads.readBytes, 73728);
    EXPECT_EQ(controller.setCell(3).outcome, CommandOutcome::Applied);
}

TEST_F(FileGui, StaleAndDuplicateDialogResponsesDoNotRunCommands)
{
    firstSave(); draft("3"); action("saveAction"); const auto old = model.workflowToken();
    answer("cancel"); action("saveAction"); const auto current = model.workflowToken(); const auto id = controller.operation().id;
    model.answerWorkflow(old, "discard"); EXPECT_EQ(model.workflowToken(), current); EXPECT_TRUE(model.hasDraft());
    model.answerWorkflow(current, "apply"); model.answerWorkflow(current, "discard"); settle();
    EXPECT_DOUBLE_EQ(model.cell(), 3); EXPECT_GT(controller.operation().id, id); EXPECT_FALSE(model.dirty());
    action("saveAsAction"); const auto staleFile = model.workflowToken(); controller.setCell(4);
    model.selectProjectFile(staleFile, QUrl::fromLocalFile(path("stale"))); EXPECT_FALSE(QFile::exists(path("stale")));
    EXPECT_TRUE(model.dirty());
}

TEST_F(FileGui, RetiredFileDialogCannotAnswerForNewDialogInstance)
{
    firstSave(); action("saveAsAction");
    QPointer<QObject> previous(control("projectFileDialog"));
    const auto oldToken = model.workflowToken(); invoke("projectFileDialog", "rejected");
    action("saveAsAction"); const auto token = model.workflowToken();
    EXPECT_NE(token, oldToken); EXPECT_NE(control("projectFileDialog"), previous.data());
    const auto id = controller.operation().id;
    if (previous) {
        previous->setProperty("selectedFile", QUrl::fromLocalFile(path("stale")));
        ASSERT_TRUE(QMetaObject::invokeMethod(previous, "accepted", Qt::DirectConnection));
        ASSERT_TRUE(QMetaObject::invokeMethod(previous, "rejected", Qt::DirectConnection));
    }
    EXPECT_EQ(model.workflowToken(), token); EXPECT_EQ(controller.operation().id, id);
    choose(path("new")); settle(); EXPECT_EQ(model.projectPath(), path("new"));
    EXPECT_FALSE(QFile::exists(path("stale")));
}

TEST_F(FileGui, SaveOldSnapshotPreservesLaterEditsAndDraftCursorSelection)
{
    firstSave(); modified(); Event entered, release;
    SaveTestAccess::configure(controller, [&](persistence::SavePhase phase) {
        if (phase == persistence::SavePhase::BeforeCommit) { entered.set(); release.wait(); }
    });
    action("saveAction"); ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return entered.isSet(); }));
    controller.setCell(3); draft("1e-"); click("applyButton");
    auto *field = control("cellInput"); field->setProperty("cursorPosition", 2);
    QMetaObject::invokeMethod(field, "select", Qt::DirectConnection, Q_ARG(int, 1), Q_ARG(int, 3));
    const auto cursor = field->property("cursorPosition"), start = field->property("selectionStart"), end = field->property("selectionEnd");
    const auto error = model.errorMessage(); release.set(); settle();
    EXPECT_DOUBLE_EQ(model.cell(), 3); EXPECT_TRUE(model.dirty()); EXPECT_TRUE(controller.canUndo());
    EXPECT_EQ(field->property("text").toString(), "1e-"); EXPECT_EQ(field->property("cursorPosition"), cursor);
    EXPECT_EQ(field->property("selectionStart"), start); EXPECT_EQ(field->property("selectionEnd"), end); EXPECT_EQ(model.errorMessage(), error);
    EXPECT_TRUE(model.fileMessage().contains(QStringLiteral("более поздние")));
}

TEST_F(FileGui, FailedOrUncertainSaveDoesNotContinueClose)
{
    firstSave(); modified();
    SaveTestAccess::configure(controller, {}, {}, [](QSaveFile &) { return false; });
    action("closeProjectAction"); answer("save"); settle();
    EXPECT_TRUE(controller.unconfirmedSave()); EXPECT_TRUE(model.hasProject()); EXPECT_TRUE(model.dirty());
    EXPECT_TRUE(window->isVisible()); EXPECT_FALSE(model.canSave()); EXPECT_FALSE(model.workflowPending());
    EXPECT_TRUE(model.fileMessage().contains(QStringLiteral("неопределён")));
}

TEST_F(FileGui, CancelSavePathStopsCloseAndPreservesDraftResolution)
{
    draft("0,5"); action("closeProjectAction"); answer("apply"); EXPECT_EQ(model.workflowPrompt(), "dirty");
    answer("save"); EXPECT_EQ(model.workflowPrompt(), "save"); invoke("projectFileDialog", "rejected");
    EXPECT_TRUE(model.hasProject()); EXPECT_TRUE(model.dirty()); EXPECT_DOUBLE_EQ(model.cell(), .5); EXPECT_TRUE(window->isVisible());
}

TEST_F(FileGui, CancelActiveSaveDoesNotContinueClose)
{
    firstSave(); modified(); Event entered, release;
    SaveTestAccess::configure(controller, [&](persistence::SavePhase phase) {
        if (phase == persistence::SavePhase::ManifestPrepare) { entered.set(); release.wait(); }
    });
    action("closeProjectAction"); answer("save"); entered.wait(); model.cancelOperation(); release.set(); settle();
    EXPECT_TRUE(model.hasProject()); EXPECT_TRUE(model.dirty()); EXPECT_FALSE(model.workflowPending());
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
}

TEST_F(FileGui, CloseDuringPermittedSaveWaitsAndReasksForLaterChanges)
{
    firstSave(); modified(); Event entered, release;
    SaveTestAccess::configure(controller, [&](persistence::SavePhase phase) {
        if (phase == persistence::SavePhase::BeforeCommit) { entered.set(); release.wait(); }
    });
    action("saveAction"); ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return entered.isSet(); }));
    // Repeated close waits for this workflow's Save before resolving the close request.
    window->close(); EXPECT_TRUE(window->isVisible()); EXPECT_EQ(controller.shutdownState(), ShutdownState::Open);
    controller.setCell(3); release.set(); settle();
    EXPECT_TRUE(window->isVisible()); EXPECT_TRUE(model.dirty());
    EXPECT_EQ(model.workflowPrompt(), "dirty"); answer("cancel"); EXPECT_TRUE(window->isVisible());
}

class CloseRoutes : public FileGui, public testing::WithParamInterface<int> {};
TEST_P(CloseRoutes, DraftAndDirtyCannotBypassCancelOrDiscard)
{
    firstSave(); modified(); draft("1e-");
    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()); ASSERT_TRUE(app);
    ApplicationShutdown shutdown(*app, controller, *window);
    const auto close = [&] {
        switch (GetParam()) {
        case 0: action("closeProjectAction"); break;
        case 1: window->close(); break;
        case 2: action("exitAction"); break;
        case 3: { QEvent quit(QEvent::Quit); QCoreApplication::sendEvent(app, &quit); QCoreApplication::processEvents(); break; }
        }
    };
    close(); EXPECT_EQ(model.workflowPrompt(), "draft"); answer("cancel");
    EXPECT_TRUE(model.hasProject()); EXPECT_TRUE(model.hasDraft()); EXPECT_EQ(controller.shutdownState(), ShutdownState::Open);
    close(); answer("discard"); EXPECT_EQ(model.workflowPrompt(), "dirty"); invoke("decisionDialog", "close");
    EXPECT_TRUE(model.hasProject()); EXPECT_TRUE(model.dirty()); EXPECT_EQ(controller.shutdownState(), ShutdownState::Open);
    close(); answer("discard");
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller);
        return GetParam() == 0 ? !model.hasProject() : controller.shutdownState() == ShutdownState::Ready && !window->isVisible(); }));
    EXPECT_EQ(window->isVisible(), GetParam() == 0); if (GetParam() == 0) EXPECT_TRUE(model.actionsEnabled());
}
INSTANTIATE_TEST_SUITE_P(ProjectSystemMenuQuit, CloseRoutes, testing::Range(0, 4));

class ActiveSaveClose : public FileGui, public testing::WithParamInterface<std::tuple<int, bool>> {};
TEST_P(ActiveSaveClose, ExistingPermittedSaveMustFinishBeforeCloseDecision)
{
    const auto [route, later] = GetParam();
    firstSave(); modified(); ControlledWork barrier;
    SaveTestAccess::configure(controller, [&](persistence::SavePhase phase) {
        if (phase == persistence::SavePhase::BeforeCommit) { barrier.entered.set(); barrier.complete.wait(); }
    });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return barrier.entered.isSet(); }));
    ASSERT_EQ(controller.operation().decision, PublicationDecision::Accept);
    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()); ASSERT_TRUE(app);
    ApplicationShutdown shutdown(*app, controller, *window);
    switch (route) {
    case 0: action("closeProjectAction"); break;
    case 1: window->close(); break;
    case 2: action("exitAction"); break;
    case 3: { QEvent quit(QEvent::Quit); QCoreApplication::sendEvent(app, &quit); QCoreApplication::processEvents(); break; }
    }
    EXPECT_TRUE(window->isVisible()); EXPECT_TRUE(model.hasProject());
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Open);
    if (later) { controller.setCell(3); draft("1e-"); }
    barrier.complete.set();
    if (later) {
        ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return model.workflowPrompt() == "draft"; }));
        EXPECT_TRUE(model.dirty()); answer("cancel");
        EXPECT_TRUE(model.hasProject()); EXPECT_TRUE(window->isVisible()); EXPECT_TRUE(model.hasDraft());
        EXPECT_DOUBLE_EQ(model.cell(), 3); EXPECT_TRUE(controller.canUndo());
        EXPECT_EQ(controller.shutdownState(), ShutdownState::Open);
    } else {
        ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller);
            return route == 0 ? !model.hasProject() : !window->isVisible(); }));
        if (route == 0) EXPECT_TRUE(window->isVisible());
        else EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    }
}
INSTANTIATE_TEST_SUITE_P(ProjectSystemMenuQuitWithAndWithoutLaterEdits, ActiveSaveClose,
    testing::Combine(testing::Range(0, 4), testing::Bool()));

class OpenWhileBusy : public FileGui, public testing::WithParamInterface<bool> {};
TEST_P(OpenWhileBusy, NoProjectEpochIsCheckedEvenWhenIntentBeginsDuringWorker)
{
    firstSave(); action("closeProjectAction"); settle(); ASSERT_FALSE(model.hasProject());
    const auto source = area.filePath("waiting.xyz");
    QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write("1 2 3\n"), 6); file.close();
    ASSERT_EQ(controller.importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return controller.pendingNonZeroZ().has_value(); }));
    action("openAction"); choose(path()); EXPECT_TRUE(model.workflowPending());
    if (GetParam()) ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied); // NoProject -> NoProject, new epoch
    else controller.cancelOperation();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return !model.workflowPending(); })); settle();
    EXPECT_EQ(model.hasProject(), !GetParam());
    if (GetParam()) EXPECT_TRUE(model.fileMessage().contains(QStringLiteral("Контекст")));
    else EXPECT_FALSE(model.dirty());
}
INSTANTIATE_TEST_SUITE_P(CancelWorkerOrCloseNoProject, OpenWhileBusy, testing::Bool());

TEST_F(FileGui, OpenSelectionCancellationPreservesInvalidDraftAndError)
{
    firstSave(); modified(); draft("1e-"); click("applyButton");
    const auto error = model.errorMessage(); const auto stamp = controller.project()->operationStamp();
    action("openAction"); const auto token = model.workflowToken(); invoke("projectFileDialog", "rejected");
    model.selectProjectFile(token, QUrl::fromLocalFile(path()));
    EXPECT_TRUE(controller.project()->accepts(stamp)); EXPECT_EQ(model.errorMessage(), error);
    EXPECT_EQ(control("cellInput")->property("text").toString(), "1e-"); EXPECT_TRUE(controller.canUndo());
}

TEST_F(FileGui, CorruptCacheShowsPartialStateWithoutValidationOrRebuild)
{
    firstSave(); const auto cachePath = QString::fromStdWString(controller.project()->dataset()->cachePath().wstring());
    QFile cache(cachePath); ASSERT_TRUE(cache.open(QIODevice::ReadWrite)); ASSERT_EQ(cache.write("broken"), 6); cache.close();
    action("openAction"); choose(path()); settle();
    EXPECT_TRUE(model.hasProject()); EXPECT_FALSE(model.cacheAvailable()); EXPECT_FALSE(model.canSave());
    EXPECT_TRUE(model.cacheStatusText().contains(QStringLiteral("повреждён")));
    EXPECT_EQ(controller.setCell(3).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.saveProject().outcome, CommandOutcome::Rejected);
    click("recheckCacheButton"); settle();
    EXPECT_FALSE(model.cacheAvailable()); EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0);
}

TEST_F(FileGui, NestedNotificationsConsumePromptOnceAndRetainBusyIntent)
{
    firstSave(); modified(); QObject observer; int starts = 0; OperationId last = 0;
    QObject::connect(&controller, &ProjectController::operationChanged, &observer, [&] {
        if (controller.operation().kind == OperationKind::Save && controller.operation().id != last) {
            last = controller.operation().id; ++starts;
        }
        QCoreApplication::processEvents();
    });
    QObject::connect(&model, &ProjectViewModel::workflowChanged, &observer, [&] {
        if (model.workflowPrompt() == "dirty") { const auto token = model.workflowToken(); model.answerWorkflow(token, "save"); model.answerWorkflow(token, "discard"); }
    });
    action("closeProjectAction");
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return !model.hasProject(); }));
    EXPECT_EQ(starts, 1); EXPECT_TRUE(window->isVisible());
}

TEST_F(FileGui, AutosaveDoesNotAcceptDraftOrClearDirtyAndExplicitSaveSurvivesBusy)
{
    firstSave(); modified(); draft("0,5");
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied);
    action("saveAction"); EXPECT_EQ(model.workflowPrompt(), "draft"); answer("apply");
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return !model.workflowPending(); })); settle();
    EXPECT_DOUBLE_EQ(model.cell(), .5); EXPECT_FALSE(model.dirty()); EXPECT_FALSE(model.hasDraft());
    modified(); draft("bad"); ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied); settle();
    EXPECT_TRUE(model.dirty()); EXPECT_EQ(control("cellInput")->property("text").toString(), "bad");
}

namespace {
class DeferredImportScenario : public FileGui {
protected:
    ControlledWork barrier;
    QObject observer;
    OperationStamp stamp{};
    OperationId autosaveId = 0, started = 0, finishedImport = 0;
    QString incoming, selection, dirtyDecision;
    int imports = 0, importCompletions = 0;
    bool dispatchNotified = false;

    void TearDown() override
    {
        barrier.complete.set();
        FileGui::TearDown();
    }
    void prepare(bool afterPermission, std::function<void()> atDispatch = {})
    {
        firstSave(); modified(); stamp = controller.project()->operationStamp();
        incoming = area.filePath("next.xyz");
        QFile file(incoming); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("8 9 0\n"), 6); file.close();
        click("newProjectButton"); selection = model.selectionToken();
        control("importFileDialog")->setProperty("selectedFile", QUrl::fromLocalFile(incoming));
        invoke("importFileDialog", "accepted");
        if (afterPermission) OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, {},
            [&](PublicationDecision decision) { if (decision == PublicationDecision::Accept) {
                barrier.entered.set(); barrier.complete.wait(); } });
        ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied);
        autosaveId = controller.operation().id;
        const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_TRUE(waitContext(context, true));
        if (afterPermission) { OperationTestAccess::poll(controller); barrier.entered.wait(); }
        QObject::connect(&controller, &ProjectController::operationChanged, &observer, [&] {
            const auto &operation = controller.operation();
            if (operation.kind != OperationKind::Import) return;
            if (operation.id != started) { started = operation.id; ++imports; }
            if (!operation.active && operation.id != finishedImport) { finishedImport = operation.id; ++importCompletions; }
        });
        if (atDispatch) QObject::connect(&model, &ProjectViewModel::importUiChanged, &observer, [&, atDispatch] {
            if (dispatchNotified || model.selectionToken() != selection || model.replacementPending()
                || !model.workflowPrompt().isEmpty()) return;
            dispatchNotified = true;
            atDispatch();
        });
        click("replaceConfirmButton"); ASSERT_EQ(model.workflowPrompt(), "dirty");
        dirtyDecision = model.workflowToken(); click("decisionDiscardButton");
        ASSERT_TRUE(control("cellInput")->property("enabled").toBool());
    }
    void expectPreserved(const QString &input)
    {
        ASSERT_TRUE(controller.project()); EXPECT_TRUE(controller.project()->accepts(stamp));
        EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(model.dirty());
        EXPECT_EQ(model.cellDraft(), input); EXPECT_EQ(control("cellInput")->property("text").toString(), input);
        EXPECT_EQ(model.selectionToken(), selection); EXPECT_EQ(model.replacementFileName(), "next.xyz");
        EXPECT_EQ(imports, 0);
    }
    void awaitDraft()
    {
        barrier.complete.set();
        ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller);
            return model.workflowPrompt() == "draft" || (model.sourceName() == "next.xyz" && !controller.operation().active); }));
        EXPECT_EQ(model.workflowPrompt(), "draft");
    }
};
}

class DeferredImportDraft : public DeferredImportScenario, public testing::WithParamInterface<bool> {};
TEST_P(DeferredImportDraft, R1NewDraftBeforeAdmissionRequiresAnotherDecision)
{
    prepare(GetParam()); draft("1e-");
    awaitDraft(); expectPreserved("1e-");
    if (model.workflowPrompt() == "draft") answer("cancel");
}
INSTANTIATE_TEST_SUITE_P(BeforeAndAfterAutosavePermission, DeferredImportDraft, testing::Bool());

class DeferredImportChoices : public DeferredImportScenario, public testing::WithParamInterface<std::tuple<bool, bool, int>> {};
TEST_P(DeferredImportChoices, R1LatestDraftAndPersistentChangesNeedTheirOwnConsent)
{
    const auto [afterPermission, valid, decision] = GetParam();
    prepare(afterPermission);
    const auto input = valid ? QStringLiteral("3") : QStringLiteral("1e-");
    draft(input); auto *field = control("cellInput");
    QMetaObject::invokeMethod(field, "select", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 1));
    const auto cursor = field->property("cursorPosition"), start = field->property("selectionStart"), end = field->property("selectionEnd");
    awaitDraft(); expectPreserved(input);
    EXPECT_EQ(field->property("cursorPosition"), cursor); EXPECT_EQ(field->property("selectionStart"), start); EXPECT_EQ(field->property("selectionEnd"), end);
    const auto token = model.workflowToken();
    model.answerWorkflow(dirtyDecision, "discard");
    model.confirmReplacement(selection, true); model.selectImportFile(selection, QUrl::fromLocalFile(incoming));
    EXPECT_EQ(model.workflowToken(), token); expectPreserved(input);
    if (decision == 0) {
        click("decisionAcceptButton");
        if (!valid) {
            EXPECT_EQ(model.workflowPrompt(), "draft"); EXPECT_NE(model.workflowToken(), token);
            EXPECT_FALSE(model.errorMessage().isEmpty()); expectPreserved(input);
            model.answerWorkflow(token, "discard"); EXPECT_EQ(model.workflowPrompt(), "draft");
            click("decisionCancelButton");
        } else {
            EXPECT_DOUBLE_EQ(model.cell(), 3); EXPECT_EQ(model.workflowPrompt(), "dirty");
            EXPECT_EQ(imports, 0); EXPECT_EQ(model.selectionToken(), selection);
            model.answerWorkflow(dirtyDecision, "discard"); model.answerWorkflow(token, "discard");
            EXPECT_EQ(model.workflowPrompt(), "dirty"); EXPECT_EQ(imports, 0);
            click("decisionDiscardButton");
        }
    } else if (decision == 1) click("decisionDiscardButton");
    else click("decisionCancelButton");
    const bool allowed = decision == 1 || (decision == 0 && valid);
    if (allowed) {
        ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return model.sourceName() == "next.xyz" && !controller.operation().active; }));
        EXPECT_EQ(imports, 1); EXPECT_EQ(importCompletions, 1); EXPECT_NE(controller.project()->runtimeIdentity(), stamp.projectIdentity);
        EXPECT_FALSE(controller.canUndo()); EXPECT_EQ(model.cellDraft(), "1");
    } else {
        settle(); EXPECT_TRUE(controller.project()->accepts(stamp)); EXPECT_TRUE(model.dirty());
        EXPECT_EQ(model.cellDraft(), input); EXPECT_TRUE(controller.canUndo());
        EXPECT_FALSE(model.workflowPending()); EXPECT_TRUE(model.selectionToken().isEmpty()); EXPECT_EQ(imports, 0);
        if (!valid && decision == 0) { draft("4"); click("applyButton"); EXPECT_DOUBLE_EQ(model.cell(), 4); EXPECT_EQ(imports, 0); }
    }
    model.answerWorkflow(token, "discard"); model.confirmReplacement(selection, true);
    QCoreApplication::processEvents(); EXPECT_EQ(imports, allowed ? 1 : 0);
}
INSTANTIATE_TEST_SUITE_P(PermissionValidityAndApplyDiscardCancel, DeferredImportChoices,
    testing::Combine(testing::Bool(), testing::Bool(), testing::Range(0, 3)));

class DeferredImportReentry : public DeferredImportScenario, public testing::WithParamInterface<std::tuple<bool, int>> {};
TEST_P(DeferredImportReentry, R1NestedNotificationAndStaleAnswersCannotAdmitNewDraft)
{
    const auto [afterPermission, delivery] = GetParam();
    const auto inject = [&] {
        draft("1e-");
        for (int i = 0; i != 8; ++i) {
            model.answerWorkflow(dirtyDecision, "discard"); model.confirmReplacement(selection, true);
            QCoreApplication::processEvents();
        }
    };
    prepare(afterPermission, delivery == 0 ? std::function<void()>(inject) : std::function<void()>{});
    bool terminalNotified = false;
    if (delivery == 1) QObject::connect(&controller, &ProjectController::operationChanged, &observer, [&] {
        if (terminalNotified || controller.operation().id != autosaveId || controller.operation().active) return;
        terminalNotified = true; inject();
    });
    if (delivery >= 2) draft("3");
    awaitDraft();
    if (delivery == 0) EXPECT_TRUE(dispatchNotified);
    if (delivery == 1) EXPECT_TRUE(terminalNotified);
    const auto token = model.workflowToken();
    if (delivery == 2) {
        draft("1e-"); model.answerWorkflow(token, "discard");
        EXPECT_NE(model.workflowToken(), token); EXPECT_EQ(model.workflowPrompt(), "draft");
    }
    bool appliedNotified = false;
    if (delivery == 3) {
        QObject::connect(&model, &ProjectViewModel::stateChanged, &observer, [&] {
            if (appliedNotified || model.cell() != 3) return;
            appliedNotified = true; inject();
        });
        click("decisionAcceptButton");
        EXPECT_TRUE(appliedNotified); EXPECT_DOUBLE_EQ(model.cell(), 3);
        EXPECT_EQ(controller.project()->runtimeIdentity(), stamp.projectIdentity);
        stamp = controller.project()->operationStamp();
        EXPECT_EQ(model.workflowPrompt(), "draft");
    }
    model.answerWorkflow(dirtyDecision, "discard");
    if (delivery == 2) { model.answerWorkflow(token, "apply"); model.answerWorkflow(token, "cancel"); }
    expectPreserved("1e-");
    if (delivery == 3) {
        click("decisionDiscardButton"); EXPECT_EQ(model.workflowPrompt(), "dirty");
        EXPECT_EQ(imports, 0); EXPECT_EQ(model.selectionToken(), selection);
    }
    click("decisionCancelButton"); settle(); EXPECT_EQ(imports, 0);
}
INSTANTIATE_TEST_SUITE_P(PermissionAndPreDispatchTerminalStaleReplyOrApply, DeferredImportReentry,
    testing::Combine(testing::Bool(), testing::Range(0, 4)));

class FailedSaveContinuation : public FileGui, public testing::WithParamInterface<int> {};
TEST_P(FailedSaveContinuation, FailedManifestWriteCannotContinueCloseOpenOrImport)
{
    firstSave(); modified(); const auto stamp = controller.project()->operationStamp();
    const auto baseline = controller.savedProject()->snapshot.snapshotId;
    ControlledWork barrier;
    SaveTestAccess::configure(controller, [&](persistence::SavePhase phase) {
        if (phase == persistence::SavePhase::ManifestWrite) { barrier.entered.set(); barrier.complete.wait(); }
    }, [](QIODevice &, const char *, qint64) -> qint64 { return -1; });
    QString selection;
    if (GetParam() == 0) action("closeProjectAction");
    else if (GetParam() == 1) { action("openAction"); choose(path()); }
    else {
        const auto incoming = area.filePath("after-failure.xyz");
        QFile file(incoming); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write("8 9 0\n"), 6); file.close();
        click("newProjectButton"); selection = model.selectionToken();
        control("importFileDialog")->setProperty("selectedFile", QUrl::fromLocalFile(incoming)); invoke("importFileDialog", "accepted");
        click("replaceConfirmButton");
    }
    ASSERT_EQ(model.workflowPrompt(), "dirty"); const auto oldDecision = model.workflowToken();
    click("decisionAcceptButton"); barrier.entered.wait();
    const auto saveId = controller.operation().id;
    draft("1e-"); click("applyButton"); const auto error = model.errorMessage();
    auto *field = control("cellInput"); QMetaObject::invokeMethod(field, "select", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 2));
    const auto cursor = field->property("cursorPosition"), start = field->property("selectionStart"), end = field->property("selectionEnd");
    barrier.complete.set();
    settle();
    EXPECT_EQ(controller.operation().id, saveId); EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    ASSERT_TRUE(controller.operation().failure); ASSERT_TRUE(controller.operation().failure->saveError);
    EXPECT_EQ(controller.operation().failure->saveError->code, persistence::SaveErrorCode::FileIO);
    EXPECT_FALSE(controller.unconfirmedSave()); EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, baseline);
    EXPECT_TRUE(controller.project()->accepts(stamp)); EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(model.dirty());
    EXPECT_EQ(model.cellDraft(), "1e-"); EXPECT_EQ(model.errorMessage(), error); EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(field->property("cursorPosition"), cursor); EXPECT_EQ(field->property("selectionStart"), start); EXPECT_EQ(field->property("selectionEnd"), end);
    EXPECT_TRUE(window->isVisible()); EXPECT_FALSE(model.workflowPending()); EXPECT_TRUE(model.canSave());
    EXPECT_TRUE(model.fileMessage().contains(QStringLiteral("Ошибка")));
    EXPECT_FALSE(model.fileMessage().contains(QStringLiteral("Проект сохранён.")));
    model.answerWorkflow(oldDecision, "save"); model.confirmReplacement(selection, true);
    QCoreApplication::processEvents(); EXPECT_EQ(controller.operation().id, saveId);
    SaveTestAccess::configure(controller);
    action("saveAction"); ASSERT_EQ(model.workflowPrompt(), "draft"); click("decisionDiscardButton"); settle();
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded); EXPECT_FALSE(model.dirty());
    EXPECT_EQ(controller.project()->runtimeIdentity(), stamp.projectIdentity); EXPECT_TRUE(controller.canUndo());
    EXPECT_EQ(model.sourceName(), "source.xyz"); EXPECT_TRUE(model.fileMessage().contains(QStringLiteral("Проект сохранён.")));
}
INSTANTIATE_TEST_SUITE_P(CloseOpenImport, FailedSaveContinuation, testing::Range(0, 3));
