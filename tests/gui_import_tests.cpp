#include "ui/project_view_model.h"
#include "operation_test_support.h"
#include "project_test_access.h"
#include "recovery_test_access.h"

#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <limits>
#include <tuple>
#include <vector>

using namespace cadcontour;
using namespace cadcontour::test;

namespace {
class GuiImport : public testing::Test {
protected:
    QTemporaryDir area;
    QString root = area.filePath("storage");
    ProjectController controller{root};
    ProjectViewModel model{controller};
    std::vector<QString> warnings;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;

    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        QQmlEngine::setObjectOwnership(&model, QQmlEngine::CppOwnership);
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.push_back(error.toString());
        });
        engine.setInitialProperties({{"project", QVariant::fromValue(&model)}, {"fileDialogEnabled", false}});
        engine.loadFromModule("CADContour2D", "Main");
        ASSERT_EQ(engine.rootObjects().size(), 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        ASSERT_NE(window, nullptr);
        ASSERT_TRUE(spinUntil([&] { return model.selectingFile(); }));
    }
    void TearDown() override
    {
        controller.cancelOperation();
        EXPECT_TRUE(finish(controller));
        for (const auto &warning : warnings) ADD_FAILURE() << warning.toStdString();
    }
    QObject *control(const char *name)
    {
        auto *object = window->findChild<QObject *>(QString::fromLatin1(name));
        EXPECT_NE(object, nullptr) << name;
        return object;
    }
    QString text(const char *name) { return control(name)->property("text").toString(); }
    bool visible(const char *name) { return control(name)->property("visible").toBool(); }
    bool enabled(const char *name) { return control(name)->property("enabled").toBool(); }
    void invoke(const char *name, const char *method)
    { ASSERT_TRUE(QMetaObject::invokeMethod(control(name), method, Qt::DirectConnection)); }
    void click(const char *name) { invoke(name, "clicked"); }
    QString source(const QByteArray &bytes = "1 2 0\n3 4 0\n", const QString &name = QStringLiteral("source.xyz"))
    {
        const auto path = area.filePath(name);
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write(bytes), bytes.size());
        return path;
    }
    void choose(const QUrl &url)
    {
        ASSERT_TRUE(control("importFileDialog")->setProperty("selectedFile", url));
        invoke("importFileDialog", "accepted"); // real production onAccepted -> QUrl boundary -> worker
    }
    void select(const QString &path) { choose(QUrl::fromLocalFile(path)); }
    void initial()
    {
        select(source());
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        ASSERT_NE(controller.project(), nullptr);
    }
    void history()
    {
        control("cellInput")->setProperty("text", "2"); click("applyButton");
        control("cellInput")->setProperty("text", "3"); click("applyButton");
        click("undoButton");
        ASSERT_TRUE(controller.canUndo()); ASSERT_TRUE(controller.canRedo());
    }
    void waitZ()
    {
        ASSERT_TRUE(spinUntil([&] { return visible("nonZeroZDialog"); }));
        auto c = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_NE(c, nullptr);
        std::unique_lock lock(c->mutex);
        ASSERT_TRUE(c->decisionChanged.wait_for(lock, std::chrono::seconds(10), [&] { return c->waitingForZ; }));
    }
    void beginReplacement(const QString &path)
    {
        ASSERT_TRUE(finishBackgroundAutosave(controller));
        click("newProjectButton"); select(path);
        ASSERT_TRUE(visible("replacementDialog"));
    }
};
}

TEST_F(GuiImport, StartupCancelAndNewButtonOfferExactlyOneSelection)
{
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(text("projectStatus"), QStringLiteral("Нет открытого проекта"));
    EXPECT_FALSE(visible("datasetCard"));
    const auto token = model.selectionToken();
    invoke("importFileDialog", "rejected");
    QCoreApplication::processEvents();
    model.offerStartupImport();
    EXPECT_TRUE(window->isVisible()); EXPECT_FALSE(model.selectingFile());
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_TRUE(enabled("newProjectButton"));
    click("newProjectButton");
    EXPECT_TRUE(model.selectingFile()); EXPECT_NE(token, model.selectionToken());
}

TEST_F(GuiImport, FirstImportUsesAcceptedMetadataAndRetainsTerminalSuccess)
{
    const auto path = source("-1.2345678901234567 1e-100 0\n1e100 -7.125 0\n");
    select(path);
    ASSERT_TRUE(finish(controller));
    ASSERT_NE(controller.project(), nullptr);
    EXPECT_TRUE(visible("datasetCard"));
    EXPECT_EQ(text("sourceNameLabel"), QStringLiteral("source.xyz"));
    EXPECT_EQ(text("pointCountLabel"), QStringLiteral("Точек: 2"));
    EXPECT_TRUE(text("boundsLabel").contains("-1.2345678901234567"));
    EXPECT_TRUE(text("boundsLabel").contains("1e+100"));
    EXPECT_TRUE(text("boundsLabel").contains("1e-100"));
    EXPECT_TRUE(text("cacheStatusLabel").contains(QStringLiteral("Point Cache готов")));
    EXPECT_EQ(model.cellText(), "1");
    EXPECT_FALSE(controller.canUndo()); EXPECT_FALSE(controller.canRedo());
    EXPECT_EQ(model.densityMapStatusText(), QStringLiteral("Карта плотности отсутствует"));
    EXPECT_TRUE(model.operationVisible());
    EXPECT_TRUE(text("operationLabel").contains(QStringLiteral("Импорт завершён")));
    EXPECT_TRUE(QFile::remove(path));
    EXPECT_TRUE(model.hasDataset()); // source independence
}

class PathImport : public GuiImport, public testing::WithParamInterface<const char *> {};
TEST_P(PathImport, LocalFileUrlRoundTripsThroughProductionFileDialogHandler)
{
    const auto name = QString::fromUtf8(GetParam());
    const auto path = source("1 2 0\n", name);
    const auto url = QUrl::fromLocalFile(path);
    select(path);
    ASSERT_TRUE(finish(controller));
    ASSERT_NE(controller.project(), nullptr);
    EXPECT_EQ(model.sourceName(), name);
    EXPECT_EQ(QString::fromStdWString(controller.project()->dataset()->sourcePath.wstring()), url.toLocalFile());
}
INSTANTIATE_TEST_SUITE_P(Names, PathImport, testing::Values("точки.xyz", "cloud with spaces.asc",
    "cloud%20literal.xyz", "cloud #1 + 100%.xyz"));

class InvalidSelection : public GuiImport, public testing::WithParamInterface<const char *> {};
TEST_P(InvalidSelection, RejectsInvalidUrlWithoutStartingWorker)
{
    // Native FileDialog rejects these itself; exercise the C++ UI boundary independently.
    model.selectImportFile(model.selectionToken(), QUrl::fromEncoded(GetParam()));
    EXPECT_EQ(controller.operation().id, 0);
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_FALSE(model.importErrorMessage().isEmpty());
    EXPECT_TRUE(model.canImport());
}
INSTANTIATE_TEST_SUITE_P(Urls, InvalidSelection, testing::Values("", "https://example.com/source.xyz",
    "file:relative.xyz", "file:///C:/source.txt", "file:///C:/source.xyz?query",
    "file:///C:/source.xyz#fragment", "file:///C:/bad%00.xyz", "file://user@host/share/source.xyz"));

class FailedImport : public GuiImport, public testing::WithParamInterface<const char *> {};
TEST_P(FailedImport, ReportsStructuredFailureWithoutCreatingProject)
{
    select(source(GetParam()));
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_TRUE(model.operationVisible());
    EXPECT_FALSE(text("operationLabel").isEmpty());
    EXPECT_FALSE(model.importDiagnostic().isEmpty());
    EXPECT_TRUE(visible("importDiagnosticLabel"));
}
INSTANTIATE_TEST_SUITE_P(Content, FailedImport, testing::Values("", "1 bad 0\n", "1 2\n", "1e999 2 0\n"));

TEST_F(GuiImport, MissingAndUnreadableSourceFailInWorker)
{
    const auto path = source("1 2 0\n", "missing.xyz");
    ASSERT_TRUE(control("importFileDialog")->setProperty("selectedFile", QUrl::fromLocalFile(path)));
    ASSERT_TRUE(QFile::remove(path)); // source disappears after selection, before acceptance
    invoke("importFileDialog", "accepted");
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(model.importDiagnostic().value("code").toString(), "OpenFailed");
    const auto directory = area.filePath("directory.xyz");
    ASSERT_TRUE(QDir().mkdir(directory));
    click("newProjectButton"); model.selectImportFile(model.selectionToken(), QUrl::fromLocalFile(directory));
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.project(), nullptr);
}

TEST_F(GuiImport, DiagnosticPreservesPhysicalLineColumnCoordinateAndToken)
{
    select(source("\n1 2 0\n3 broken 0\n"));
    ASSERT_TRUE(finish(controller));
    const auto fields = model.importDiagnostic();
    EXPECT_EQ(fields.value("code").toString(), "ParseError");
    EXPECT_EQ(fields.value("phase").toString(), "Parse");
    EXPECT_EQ(fields.value("line").toString(), "3");
    EXPECT_EQ(fields.value("column").toString(), "3");
    EXPECT_EQ(fields.value("coordinate").toString(), "Y");
    EXPECT_EQ(fields.value("token").toString(), "broken");
    EXPECT_TRUE(text("operationLabel").contains(QStringLiteral("Строка 3")));
}

TEST_F(GuiImport, ReplacementRejectAndSuccessPreserveOldCacheAndResetHistory)
{
    initial(); history();
    const auto stamp = controller.project()->operationStamp();
    const auto cachePath = controller.project()->dataset()->cachePath();
    const auto path = source("8 9 0\n", "next.asc");
    beginReplacement(path); click("replaceRejectButton");
    ASSERT_TRUE(spinUntil([&] { return !model.replacementPending(); }));
    EXPECT_TRUE(controller.project()->accepts(stamp));
    EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(controller.canRedo());
    beginReplacement(path); click("replaceConfirmButton");
    ASSERT_TRUE(finish(controller));
    EXPECT_NE(controller.project()->runtimeIdentity(), stamp.projectIdentity);
    EXPECT_EQ(model.sourceName(), "next.asc"); EXPECT_EQ(model.cellText(), "1");
    EXPECT_FALSE(controller.canUndo()); EXPECT_FALSE(controller.canRedo());
    EXPECT_TRUE(std::filesystem::exists(cachePath));
}

TEST_F(GuiImport, FailedReplacementPreservesDatasetHistoryAndDraftSelectionError)
{
    initial(); history();
    const auto stamp = controller.project()->operationStamp();
    control("cellInput")->setProperty("text", "1,2.3"); click("applyButton");
    const auto error = model.errorMessage();
    auto *field = control("cellInput");
    ASSERT_TRUE(QMetaObject::invokeMethod(field, "select", Q_ARG(int, 1), Q_ARG(int, 3)));
    const auto cursor = field->property("cursorPosition");
    const auto start = field->property("selectionStart");
    const auto end = field->property("selectionEnd");
    beginReplacement(source("1 wrong 0\n", "bad.xyz")); click("replaceConfirmButton");
    ASSERT_TRUE(finish(controller));
    EXPECT_TRUE(controller.project()->accepts(stamp));
    EXPECT_EQ(model.sourceName(), "source.xyz");
    EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(text("cellInput"), "1,2.3"); EXPECT_EQ(model.errorMessage(), error);
    EXPECT_EQ(field->property("cursorPosition"), cursor);
    EXPECT_EQ(field->property("selectionStart"), start); EXPECT_EQ(field->property("selectionEnd"), end);
}

TEST_F(GuiImport, CancelledReplacementKeepsOldMetadataAndHistoryUntilAccept)
{
    initial(); history();
    const auto stamp = controller.project()->operationStamp();
    beginReplacement(source("8 9 1\n", "next.xyz")); click("replaceConfirmButton"); waitZ();
    EXPECT_TRUE(controller.project()->accepts(stamp));
    EXPECT_EQ(model.sourceName(), "source.xyz");
    EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(controller.canRedo());
    click("cancelZButton");
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_FALSE(controller.operation().failure);
    EXPECT_TRUE(controller.project()->accepts(stamp));
    EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(controller.canRedo());
}

TEST_F(GuiImport, StaleReplacementAndSelectionCallbacksCannotStartImport)
{
    initial();
    beginReplacement(source("3 4 0\n", "next.xyz"));
    const auto token = model.selectionToken(); const auto id = controller.operation().id;
    controller.setCell(2);
    model.confirmReplacement(token, true);
    EXPECT_EQ(controller.operation().id, id); EXPECT_FALSE(model.replacementPending());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2);
    click("newProjectButton");
    const auto selection = model.selectionToken();
    controller.closeProject();
    model.selectImportFile(selection, QUrl::fromLocalFile(source()));
    EXPECT_EQ(controller.operation().id, id); EXPECT_EQ(controller.project(), nullptr);
}

class AutosaveImportUi : public GuiImport, public testing::WithParamInterface<std::tuple<bool, bool>> {};
TEST_P(AutosaveImportUi, KeepsSelectionAndImportsWhenConfirmedDuringAutosave)
{
    const auto [confirmationOpen, afterAccept] = GetParam();
    initial(); ASSERT_TRUE(finishBackgroundAutosave(controller)); RecoveryTestAccess::timers(controller);
    controller.setCell(.5);
    const auto path = source("8 9 0\n", "next.xyz");
    if (!confirmationOpen) {
        QQmlExpression options(qmlContext(control("importFileDialog")), control("importFileDialog"),
            QStringLiteral("options = NativeDialogs.FileDialog.DontUseNativeDialog"));
        options.evaluate(); ASSERT_FALSE(options.hasError());
        ASSERT_TRUE(window->setProperty("fileDialogEnabled", true));
    }
    click("newProjectButton");
    const auto token = model.selectionToken(); ASSERT_FALSE(token.isEmpty());
    if (confirmationOpen) select(path);
    ControlledWork barrier;
    if (afterAccept) OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, {},
        [&](PublicationDecision decision) { if (decision == PublicationDecision::Accept) {
            barrier.entered.set(); barrier.complete.wait(); } });
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied);
    const auto autosaveId = controller.operation().id;
    const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_TRUE(waitContext(context, true)); // no poll/permission until the assertions below
    if (afterAccept) { OperationTestAccess::poll(controller); barrier.entered.wait(); }
    EXPECT_EQ(model.selectionToken(), token);
    if (confirmationOpen) {
        EXPECT_TRUE(model.replacementPending()); EXPECT_TRUE(visible("replacementDialog"));
        EXPECT_EQ(model.replacementFileName(), "next.xyz");
    } else {
        EXPECT_TRUE(model.selectingFile());
        EXPECT_TRUE(visible("importFileDialog"));
        EXPECT_EQ(control("importFileDialog")->property("requestToken").toString(), token);
        select(path);
        EXPECT_TRUE(model.replacementPending());
    }
    int completedImports = 0;
    OperationState completedImport;
    QObject observer;
    QObject::connect(&controller, &ProjectController::operationChanged, &observer, [&] {
        if (controller.operation().kind == OperationKind::Import && !controller.operation().active) {
            ++completedImports; completedImport = controller.operation();
        }
    });
    model.confirmReplacement(token, true);
    EXPECT_EQ(model.selectionToken(), token); // retained until native completion/retry
    EXPECT_EQ(controller.operation().cancellationRequested, !afterAccept);
    EXPECT_TRUE(controller.operation().active); EXPECT_EQ(controller.operation().kind, OperationKind::Autosave);
    barrier.complete.set();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller);
        return model.sourceName() == "next.xyz" && !controller.operation().active; }));
    EXPECT_EQ(completedImports, 1);
    EXPECT_EQ(completedImport.status, OperationStatus::Succeeded);
    EXPECT_EQ(completedImport.kind, OperationKind::Import); EXPECT_GT(completedImport.id, autosaveId);
    EXPECT_TRUE(model.selectionToken().isEmpty()); EXPECT_FALSE(model.replacementPending());
    const auto id = controller.operation().id;
    model.selectImportFile(token, QUrl::fromLocalFile(path)); model.confirmReplacement(token, true);
    EXPECT_EQ(controller.operation().id, id);
}
INSTANTIATE_TEST_SUITE_P(FileAndConfirmationBeforeAndAfterAccept, AutosaveImportUi,
    testing::Combine(testing::Bool(), testing::Bool()));

class NestedAutosaveImport : public GuiImport, public testing::WithParamInterface<std::tuple<bool, bool>> {};
TEST_P(NestedAutosaveImport, N1NestedEventDeliveryAtTerminalPreservesIntentAndImportsExactlyOnce)
{
    const auto [confirmationOpen, afterAccept] = GetParam();
    initial(); ASSERT_TRUE(finishBackgroundAutosave(controller)); RecoveryTestAccess::timers(controller);
    controller.setCell(.5);
    const auto path = source("8 9 0\n", "nested.xyz");
    if (!confirmationOpen) {
        QQmlExpression options(qmlContext(control("importFileDialog")), control("importFileDialog"),
            QStringLiteral("options = NativeDialogs.FileDialog.DontUseNativeDialog"));
        options.evaluate(); ASSERT_FALSE(options.hasError());
        ASSERT_TRUE(window->setProperty("fileDialogEnabled", true));
    }
    click("newProjectButton"); const auto token = model.selectionToken();
    const auto selection = controller.importContext(); ASSERT_TRUE(selection);
    if (confirmationOpen) select(path);
    ControlledWork barrier;
    if (afterAccept) OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, {},
        [&](PublicationDecision decision) { if (decision == PublicationDecision::Accept) {
            barrier.entered.set(); barrier.complete.wait(); } });
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied);
    const auto autosaveId = controller.operation().id;
    const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_TRUE(waitContext(context, true));
    if (afterAccept) { OperationTestAccess::poll(controller); barrier.entered.wait(); }
    if (!confirmationOpen) { EXPECT_TRUE(visible("importFileDialog")); select(path); }
    ASSERT_TRUE(model.replacementPending());
    model.confirmReplacement(token, true);
    ASSERT_EQ(model.selectionToken(), token);

    bool nested = false;
    int startedImports = 0;
    int completedImports = 0;
    OperationId startedImportId = 0, completedImportId = 0;
    OperationStatus importedStatus = OperationStatus::Idle;
    QObject observer;
    QObject::connect(&controller, &ProjectController::operationChanged, &observer, [&] {
        if (controller.operation().kind == OperationKind::Import) {
            if (controller.operation().id != startedImportId) {
                ++startedImports; startedImportId = controller.operation().id;
            }
            if (!controller.operation().active && controller.operation().id != completedImportId) {
                ++completedImports; completedImportId = controller.operation().id; importedStatus = controller.operation().status;
            }
        }
        if (nested || controller.operation().id != autosaveId || controller.operation().active) return;
        nested = true;
        EXPECT_FALSE(OperationTestAccess::runner(controller).busy());
        EXPECT_TRUE(context->nativeCompletion.finished());
        EXPECT_FALSE(controller.importContext());
        EXPECT_EQ(controller.importSelectionContext(), selection); // context is still applicable
        const auto rejected = controller.importAscXyz({path, {8ULL << 20, 64ULL << 20, 8}, true, selection});
        ASSERT_EQ(rejected.outcome, CommandOutcome::Rejected);
        ASSERT_TRUE(rejected.failure); EXPECT_EQ(rejected.failure->detail, ErrorDetail::ReentrantCommand);
        for (int i = 0; i != 8; ++i) {
            emit controller.operationChanged();
            QCoreApplication::processEvents(); // deliver the deferred callback inside CommandScope
        }
        EXPECT_EQ(model.selectionToken(), token);
        EXPECT_EQ(model.replacementFileName(), "nested.xyz");
        EXPECT_FALSE(model.selectingFile()); EXPECT_FALSE(model.replacementPending()); // pending explicit intent
        EXPECT_EQ(completedImports, 0);
    });
    barrier.complete.set();
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller);
        return nested && model.sourceName() == "nested.xyz" && !controller.operation().active; }));
    EXPECT_EQ(startedImports, 1); EXPECT_EQ(completedImports, 1); EXPECT_EQ(importedStatus, OperationStatus::Succeeded);
    EXPECT_TRUE(model.importErrorMessage().isEmpty()); EXPECT_TRUE(model.selectionToken().isEmpty());
    const auto id = controller.operation().id;
    for (int i = 0; i != 8; ++i) {
        emit controller.operationChanged(); QCoreApplication::processEvents();
        model.selectImportFile(token, QUrl::fromLocalFile(path)); model.confirmReplacement(token, true);
    }
    EXPECT_EQ(startedImports, 1); EXPECT_EQ(completedImports, 1); EXPECT_GE(controller.operation().id, id);
}
INSTANTIATE_TEST_SUITE_P(FileConfirmationAndPermission, NestedAutosaveImport,
    testing::Combine(testing::Bool(), testing::Bool()));

TEST_F(GuiImport, N1TransitionHintDoesNotExposeUnfinishedImportStart)
{
    ControlledWork barrier;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false,
        [&] { barrier.entered.set(); barrier.complete.wait(); });
    bool observed = false;
    QObject observer;
    QObject::connect(&controller, &ProjectController::commandTransitionFinished, &observer, [&] {
        if (observed || controller.operation().kind != OperationKind::Import || !controller.operation().active) return;
        observed = true; barrier.entered.wait();
        // A synchronous observer of the retry hint can itself run nested events/Ready.
        // startImport must already have installed importSourcePath_ after startWork.
        OperationTestAccess::poll(controller);
        for (int i = 0; i != 8; ++i) QCoreApplication::processEvents();
        barrier.complete.set();
    });
    initial();
    EXPECT_TRUE(observed); ASSERT_TRUE(controller.project());
    EXPECT_EQ(model.sourceName(), "source.xyz"); EXPECT_TRUE(model.importErrorMessage().isEmpty());
}

class InvalidatedAutosaveImport : public GuiImport, public testing::WithParamInterface<int> {};
TEST_P(InvalidatedAutosaveImport, PendingImportCannotSurviveInputCloseOrShutdownChange)
{
    initial(); ASSERT_TRUE(finishBackgroundAutosave(controller)); RecoveryTestAccess::timers(controller);
    controller.setCell(.5);
    const auto path = source("8 9 0\n", "next.xyz");
    beginReplacement(path); const auto token = model.selectionToken();
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied);
    auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_TRUE(waitContext(context, true));
    model.confirmReplacement(token, true);
    if (GetParam() == 0) { controller.setCell(2); controller.undo(); }
    else if (GetParam() == 1) controller.closeProject();
    else controller.requestShutdown();
    EXPECT_TRUE(model.selectionToken().isEmpty());
    ASSERT_TRUE(finish(controller)); QCoreApplication::processEvents();
    model.confirmReplacement(token, true); model.selectImportFile(token, QUrl::fromLocalFile(path));
    EXPECT_NE(model.sourceName(), "next.xyz"); EXPECT_NE(controller.operation().kind, OperationKind::Import);
}
INSTANTIATE_TEST_SUITE_P(InputCloseShutdown, InvalidatedAutosaveImport, testing::Range(0, 3));

TEST_F(GuiImport, CompletedAutosavePreservesSelectionUntilActualProjectReplacement)
{
    initial(); ASSERT_TRUE(finishBackgroundAutosave(controller)); RecoveryTestAccess::timers(controller);
    controller.setCell(.5); click("newProjectButton"); const auto token = model.selectionToken();
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(model.selectionToken(), token); EXPECT_TRUE(model.selectingFile());
    const auto oldIdentity = controller.project()->runtimeIdentity();
    const auto path = source("8 9 0\n", "next.xyz");
    ASSERT_EQ(controller.importAscXyz({path, {8ULL << 20, 64ULL << 20, 8}, true}).outcome, CommandOutcome::Applied);
    EXPECT_TRUE(model.selectionToken().isEmpty());
    ASSERT_TRUE(finish(controller)); RecoveryTestAccess::timers(controller);
    EXPECT_NE(controller.project()->runtimeIdentity(), oldIdentity);
    const auto id = controller.operation().id;
    model.selectImportFile(token, QUrl::fromLocalFile(path)); model.confirmReplacement(token, true);
    EXPECT_EQ(controller.operation().id, id);
}

TEST_F(GuiImport, UncUrlIsAcceptedAtUiBoundaryWithoutContactingNetwork)
{
    initial(); click("newProjectButton");
    model.selectImportFile(model.selectionToken(), QUrl("file://server/share/cloud%20%25.xyz"));
    EXPECT_TRUE(model.replacementPending());
    EXPECT_EQ(model.replacementFileName(), "cloud %.xyz");
    click("replaceRejectButton");
    ASSERT_TRUE(spinUntil([&] { return !model.replacementPending(); }));
    EXPECT_EQ(controller.operation().id, 1);
}

TEST(ImportUiBoundary, ApplicationRejectsStaleSelectionContextIncludingNoProjectAba)
{
    QTemporaryDir area;
    ProjectController controller(area.filePath("storage"));
    const auto context = controller.importContext(); ASSERT_TRUE(context);
    ProjectControllerTestAccess::newProject(controller);
    controller.closeProject();
    ImportRequest request{area.filePath("missing.xyz"), {8ULL << 20, 64ULL << 20, 8}, true, context};
    const auto result = controller.importAscXyz(request);
    EXPECT_EQ(result.outcome, CommandOutcome::Rejected);
    EXPECT_EQ(result.failure->detail, ErrorDetail::ReplacementNotConfirmed);
    EXPECT_EQ(controller.operation().id, 0);
}

TEST_F(GuiImport, NonZeroZContinueUsesCurrentTokenAndOnlyOneWarning)
{
    select(source("\n1 2 0\n3 4 5e-324\n5 6 7\n")); waitZ();
    EXPECT_TRUE(text("nonZeroZMessage").contains(QStringLiteral("строке 3")));
    EXPECT_TRUE(text("nonZeroZMessage").contains("5e-324"));
    EXPECT_TRUE(text("operationPhase").contains(QStringLiteral("Ожидается подтверждение")));
    EXPECT_TRUE(control("operationProgress")->property("indeterminate").toBool());
    bool responsive = false;
    QTimer::singleShot(0, [&] { responsive = true; });
    ASSERT_TRUE(spinUntil([&] { return responsive; }));
    const auto token = model.nonZeroZToken();
    emit controller.nonZeroZConfirmationChanged();
    EXPECT_EQ(control("nonZeroZDialog")->property("requestToken").toString(), token);
    click("continueZButton");
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_TRUE(model.datasetHasNonZeroZ()); EXPECT_EQ(model.pointCountText(), "3");
    EXPECT_FALSE(visible("nonZeroZDialog"));
    model.answerNonZeroZ(token, false);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}

class CancelZ : public GuiImport, public testing::WithParamInterface<int> {};
TEST_P(CancelZ, ButtonDialogCloseExternalCancelAndShutdownRevokeRequest)
{
    select(source("1 2 3\n")); waitZ();
    const auto token = model.nonZeroZToken();
    switch (GetParam()) {
    case 0: click("cancelZButton"); break;
    case 1: invoke("nonZeroZDialog", "close"); break;
    case 2: model.cancelOperation(); break;
    case 3: window->close(); break;
    }
    ASSERT_TRUE(finish(controller));
    ASSERT_TRUE(spinUntil([&] { return !visible("nonZeroZDialog"); }));
    EXPECT_EQ(controller.project(), nullptr); EXPECT_FALSE(controller.pendingNonZeroZ());
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_FALSE(controller.operation().failure);
    model.answerNonZeroZ(token, true);
    EXPECT_EQ(controller.project(), nullptr);
}
INSTANTIATE_TEST_SUITE_P(Routes, CancelZ, testing::Values(0, 1, 2, 3));

TEST_F(GuiImport, BusyAndClosingRejectUiCommandsAndStaleFileCallbacks)
{
    select(source("1 2 3\n")); waitZ();
    const auto id = controller.operation().id;
    EXPECT_FALSE(enabled("newProjectButton")); EXPECT_TRUE(enabled("stopOperationButton"));
    model.newProject();
    EXPECT_FALSE(model.importErrorMessage().isEmpty()); EXPECT_EQ(controller.operation().id, id);
    window->close();
    model.newProject();
    EXPECT_TRUE(model.importErrorMessage().contains(QStringLiteral("завершает")));
    EXPECT_FALSE(enabled("applyButton"));
    ASSERT_TRUE(finish(controller));
}

TEST_F(GuiImport, LateZResponseCannotResolveRequestOfNextOperation)
{
    const auto path = source("1 2 3\n"); select(path); waitZ();
    const auto old = model.nonZeroZToken(); click("cancelZButton"); ASSERT_TRUE(finish(controller));
    click("newProjectButton"); select(path); waitZ();
    EXPECT_NE(model.nonZeroZToken(), old);
    model.answerNonZeroZ(old, true);
    EXPECT_TRUE(controller.pendingNonZeroZ());
    click("continueZButton"); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}

TEST_F(GuiImport, DatasetAndDialogsResizeWithoutWarningsAndKeepCommandsAccessible)
{
    initial();
    for (const QSize size : {QSize{400, 380}, QSize{1200, 800}}) {
        window->resize(size); QCoreApplication::processEvents();
        for (const char *name : {"newProjectButton", "closeButton"}) {
            auto *item = qobject_cast<QQuickItem *>(control(name)); ASSERT_NE(item, nullptr);
            const auto rect = item->mapRectToScene(item->boundingRect());
            EXPECT_GE(rect.left(), 0); EXPECT_GE(rect.top(), 0);
            EXPECT_LE(rect.right(), window->width()); EXPECT_LE(rect.bottom(), window->height());
        }
    }
    beginReplacement(source("1 2 3\n", QString(160, u'я') + ".xyz"));
    EXPECT_LE(control("replacementDialog")->property("width").toDouble(), window->width());
    click("replaceConfirmButton"); waitZ();
    EXPECT_LE(control("nonZeroZDialog")->property("height").toDouble(), window->height());
    click("cancelZButton"); ASSERT_TRUE(finish(controller));
}

TEST(ImportUiBoundary, Uint64MetadataNeverPassesThroughQmlNumber)
{
    ProjectController controller;
    ProjectControllerTestAccess::newProject(controller);
    // Metadata-only fixture, intentionally not a real cache or publication path.
    ImportedDataset data;
    data.pointCount = std::numeric_limits<std::uint64_t>::max();
    data.minimum = {-1e100, std::numeric_limits<double>::denorm_min()};
    data.maximum = {1.2345678901234567, 1e100};
    ProjectControllerTestAccess::setDataset(controller, std::move(data));
    ProjectViewModel model(controller);
    EXPECT_EQ(model.pointCountText(), "18446744073709551615");
    EXPECT_EQ(model.property("pointCountText").metaType(), QMetaType::fromType<QString>());
    EXPECT_TRUE(model.boundsText().contains("5e-324"));
    EXPECT_TRUE(model.boundsText().contains("1.2345678901234567"));
}

TEST(ImportUiBoundary, InteractiveProfileIsBoundedAndUsesAvailableDiskWithoutOverflow)
{
    EXPECT_FALSE(interactiveImportLimits(0));
    EXPECT_FALSE(interactiveImportLimits(64ULL << 20));
    for (auto available : {1ULL << 30, 100ULL << 30, std::numeric_limits<unsigned long long>::max()}) {
        const auto result = interactiveImportLimits(available); ASSERT_TRUE(result);
        EXPECT_EQ(result.value().memoryBytes, 128ULL << 20);
        EXPECT_EQ(result.value().maxOpenFiles, 32);
        EXPECT_GT(result.value().temporaryDiskBytes, 0);
        EXPECT_LT(result.value().temporaryDiskBytes, available);
    }
}

TEST(ImportUiBoundary, ViewModelDestructionCancelsWaitingConsumerWithoutGuiEvents)
{
    QTemporaryDir area;
    QFile file(area.filePath("source.xyz")); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("1 2 3\n"); file.close();
    ProjectController controller(area.filePath("storage"));
    auto model = std::make_unique<ProjectViewModel>(controller);
    model->newProject(); model->selectImportFile(model->selectionToken(), QUrl::fromLocalFile(file.fileName()));
    const auto c = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
    ASSERT_NE(c, nullptr);
    {
        std::unique_lock lock(c->mutex);
        ASSERT_TRUE(c->decisionChanged.wait_for(lock, std::chrono::seconds(10), [&] { return c->waitingForZ; }));
    }
    model.reset();
    ASSERT_TRUE(waitContext(c, false));
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_EQ(controller.project(), nullptr);
}
