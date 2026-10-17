#include "project_test_access.h"
#include "ui/project_view_model.h"
#include "operation_test_support.h"
#include "ui/application_shutdown.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace cadcontour;

namespace {
class ProductionQml : public testing::Test {
protected:
    ProjectController controller;
    ProjectViewModel viewModel{controller};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    std::vector<QString> warnings;

    void SetUp() override
    {
        ProjectControllerTestAccess::newProject(controller);
        QQmlEngine::setObjectOwnership(&viewModel, QQmlEngine::CppOwnership);
        QObject::connect(&engine, &QQmlEngine::warnings, [&] (const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings.push_back(error.toString());
        });
        engine.setInitialProperties({{QStringLiteral("project"), QVariant::fromValue(&viewModel)}});
        engine.loadFromModule("CADContour2D", "Main");
        ASSERT_EQ(engine.rootObjects().size(), 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        ASSERT_NE(window, nullptr);
        QCoreApplication::processEvents();
        ASSERT_TRUE(warnings.empty());
    }

    void TearDown() override
    {
        for (const auto &warning : warnings)
            ADD_FAILURE() << warning.toStdString();
        // QObject connections capture this fixture; engine dies before its members.
        QObject::disconnect(&engine, nullptr, nullptr, nullptr);
    }

    QObject *control(const char *name)
    {
        auto *object = window->findChild<QObject *>(QString::fromLatin1(name));
        EXPECT_NE(object, nullptr) << name;
        return object;
    }

    QString text(const char *name) { return control(name)->property("text").toString(); }
    bool enabled(const char *name) { return control(name)->property("enabled").toBool(); }
    void draft(const QString &value) { ASSERT_TRUE(control("cellInput")->setProperty("text", value)); }
    void click(const char *name)
    {
        // Invoke the real control's signal, exercising the production onClicked handler.
        ASSERT_TRUE(QMetaObject::invokeMethod(control(name), "clicked", Qt::DirectConnection));
        QCoreApplication::processEvents();
    }
};
} // namespace

TEST_F(ProductionQml, CompleteUserScenarioAndRejectedDraft)
{
    EXPECT_TRUE(window->isVisible());
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_EQ(text("cellValueLabel"), QStringLiteral("Размер ячейки: 1 мм"));
    EXPECT_EQ(text("densityStatus"), QStringLiteral("Карта плотности отсутствует"));
    EXPECT_FALSE(enabled("undoButton"));
    EXPECT_FALSE(enabled("redoButton"));
    const auto identity = controller.project()->runtimeIdentity();
    draft(QStringLiteral("0,5"));
    click("applyButton");
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(text("cellInput"), QStringLiteral("0.5"));
    EXPECT_EQ(text("cellValueLabel"), QStringLiteral("Размер ячейки: 0.5 мм"));
    EXPECT_TRUE(enabled("undoButton"));
    EXPECT_FALSE(enabled("redoButton"));
    click("undoButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_FALSE(enabled("undoButton"));
    EXPECT_TRUE(enabled("redoButton"));
    draft(QStringLiteral("1,2.3"));
    click("applyButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("1,2.3"));
    EXPECT_FALSE(text("errorLabel").isEmpty());
    EXPECT_TRUE(control("errorLabel")->property("visible").toBool());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
    EXPECT_TRUE(enabled("redoButton"));
    draft(QStringLiteral(" 1,0 "));
    click("applyButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_TRUE(text("errorLabel").isEmpty());
    EXPECT_TRUE(enabled("redoButton"));
    click("redoButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("0.5"));
    ProjectControllerTestAccess::newProject(controller); // domain fixture transition; real import is tested separately
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_GT(controller.project()->runtimeIdentity(), identity);
    EXPECT_EQ(controller.project()->inputRevision(), 0);
    EXPECT_FALSE(enabled("undoButton"));
    EXPECT_FALSE(enabled("redoButton"));
    click("closeButton");
    EXPECT_FALSE(window->isVisible());
}

TEST_F(ProductionQml, EnterHandlerAndControllerChangesSynchronizeInput)
{
    draft(QStringLiteral("2,5"));
    ASSERT_TRUE(QMetaObject::invokeMethod(control("cellInput"), "accepted", Qt::DirectConnection));
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2.5);
    EXPECT_EQ(text("cellInput"), QStringLiteral("2.5"));
    controller.undo();
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    controller.closeProject();
    EXPECT_EQ(text("projectStatus"), QStringLiteral("Нет открытого проекта"));
    EXPECT_TRUE(text("cellInput").isEmpty());
    EXPECT_FALSE(enabled("cellInput"));
    EXPECT_FALSE(enabled("applyButton"));
    ProjectControllerTestAccess::newProject(controller); // domain fixture transition; real import is tested separately
    EXPECT_TRUE(enabled("applyButton"));
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
}

TEST_F(ProductionQml, ResizedLayoutKeepsControlsInsideWindow)
{
    draft(QStringLiteral("1,2.3"));
    click("applyButton");
    for (const QSize size : {QSize{400, 380}, QSize{1000, 700}}) {
        window->resize(size);
        QCoreApplication::processEvents();
        for (const char *name : {"cellInput", "applyButton", "errorLabel", "undoButton",
                                "redoButton", "newProjectButton", "closeButton"}) {
            auto *item = qobject_cast<QQuickItem *>(control(name));
            ASSERT_NE(item, nullptr);
            const auto bounds = item->mapRectToScene(item->boundingRect());
            EXPECT_GE(bounds.left(), 0) << name;
            EXPECT_GE(bounds.top(), 0) << name;
            EXPECT_LE(bounds.right(), window->width()) << name;
            EXPECT_LE(bounds.bottom(), window->height()) << name;
            EXPECT_GT(item->width(), 0) << name;
            EXPECT_GT(item->height(), 0) << name;
        }
    }
}

TEST_F(ProductionQml, ProgressSuccessAndUnrelatedStatePreserveDraftSelectionAndError)
{
    using namespace cadcontour::test;
    for (const auto &input : {QStringLiteral("1e-"), QStringLiteral("1,2.3")}) {
        ControlledWork work;
        draft(input);
        if (input == QStringLiteral("1,2.3"))
            click("applyButton");
        const auto error = viewModel.errorMessage();
        auto *field = control("cellInput");
        field->setProperty("cursorPosition", 2);
        ASSERT_TRUE(QMetaObject::invokeMethod(field, "select", Qt::DirectConnection,
                                            Q_ARG(int, 1), Q_ARG(int, 3)));
        const auto cursor = field->property("cursorPosition");
        const auto selectionStart = field->property("selectionStart");
        const auto selectionEnd = field->property("selectionEnd");
        OperationTestAccess::start(controller, work.function());
        work.entered.wait();
        OperationTestAccess::poll(controller);
        EXPECT_TRUE(control("operationProgress")->property("indeterminate").toBool());
        EXPECT_TRUE(enabled("stopOperationButton"));
        EXPECT_TRUE(enabled("applyButton"));
        EXPECT_FALSE(enabled("newProjectButton"));
        QObject::connect(&controller, &ProjectController::stateChanged, &engine, [&] {
            EXPECT_EQ(text("cellInput"), input);
            EXPECT_EQ(viewModel.errorMessage(), error);
        }, Qt::SingleShotConnection);
        work.complete.set();
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
        EXPECT_EQ(text("cellInput"), input);
        EXPECT_EQ(field->property("cursorPosition"), cursor);
        EXPECT_EQ(field->property("selectionStart"), selectionStart);
        EXPECT_EQ(field->property("selectionEnd"), selectionEnd);
        EXPECT_EQ(viewModel.errorMessage(), error);
        EXPECT_FALSE(enabled("stopOperationButton"));
    }
}

TEST_F(ProductionQml, CancellationAndFailurePreserveInvalidInput)
{
    using namespace cadcontour::test;
    draft(QStringLiteral("1,2.3"));
    click("applyButton");
    const auto error = text("errorLabel");
    {
        ControlledWork work;
        OperationTestAccess::start(controller, work.function());
        work.entered.wait();
        click("stopOperationButton");
        EXPECT_EQ(controller.operation().status, OperationStatus::Cancelling);
        EXPECT_FALSE(enabled("stopOperationButton"));
        work.complete.set();
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
        EXPECT_EQ(text("cellInput"), QStringLiteral("1,2.3"));
        EXPECT_EQ(text("errorLabel"), error);
    }
    OperationTestAccess::start(controller, [](const auto &, const auto &) -> WorkerOutcome {
        throw std::runtime_error("QML failure scenario");
    });
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(text("cellInput"), QStringLiteral("1,2.3"));
    EXPECT_EQ(text("errorLabel"), error);
    EXPECT_TRUE(text("operationLabel").contains(QStringLiteral("Ошибка")));
}

TEST_F(ProductionQml, KnownProgressEditingAndNewProjectDuringWork)
{
    using namespace cadcontour::test;
    Event reported, complete;
    OperationTestAccess::start(controller, [&](const auto &input, const WorkerControl &control) {
        control.report({OperationPhase::Computing, 25, 100});
        reported.set(); complete.wait();
        return success(input);
    });
    reported.wait();
    OperationTestAccess::poll(controller);
    EXPECT_FALSE(control("operationProgress")->property("indeterminate").toBool());
    EXPECT_DOUBLE_EQ(control("operationProgress")->property("value").toDouble(), 0.25);
    EXPECT_TRUE(text("operationPhase").contains(QStringLiteral("25.0 %")));
    draft(QStringLiteral("0,5"));
    ASSERT_TRUE(QMetaObject::invokeMethod(control("cellInput"), "accepted", Qt::DirectConnection));
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(text("cellInput"), QStringLiteral("0.5"));
    EXPECT_TRUE(controller.operation().publicationRevoked);
    click("undoButton");
    click("redoButton");
    const auto identity = controller.project()->runtimeIdentity();
    ProjectControllerTestAccess::newProject(controller); // domain fixture transition; real import is tested separately
    EXPECT_GT(controller.project()->runtimeIdentity(), identity);
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_TRUE(text("operationLabel").contains(QStringLiteral("предыдущего проекта")));
    for (const auto &size : {QSize{400, 380}, QSize{1000, 700}}) {
        window->resize(size);
        QCoreApplication::processEvents();
        EXPECT_GT(control("operationProgress")->property("width").toDouble(), 0);
        EXPECT_GT(control("cellInput")->property("width").toDouble(), 0);
    }
    complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_FALSE(viewModel.operationVisible());
}

TEST_F(ProductionQml, NewProjectRefreshesAcceptedAndTerminalOperationPresentation)
{
    using namespace cadcontour::test;
    Event cleanup, releaseCleanup;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false, {},
        [&](PublicationDecision) { cleanup.set(); releaseCleanup.wait(); });
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    EXPECT_TRUE(spinUntil([&] {
        OperationTestAccess::poll(controller);
        return controller.operation().decision == PublicationDecision::Accept;
    }));
    cleanup.wait();
    ProjectControllerTestAccess::newProject(controller); // domain fixture transition; real import is tested separately
    EXPECT_TRUE(text("operationLabel").contains(QStringLiteral("предыдущего проекта")));
    releaseCleanup.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_FALSE(viewModel.operationVisible());
    OperationTestAccess::start(controller, [](const auto &input, const auto &) { return success(input); });
    ASSERT_TRUE(finish(controller));
    EXPECT_TRUE(viewModel.operationVisible());
    EXPECT_TRUE(control("operationLabel")->parent()->property("visible").toBool());
    ProjectControllerTestAccess::newProject(controller); // domain fixture transition; real import is tested separately
    EXPECT_FALSE(viewModel.operationVisible());
    EXPECT_FALSE(control("operationLabel")->parent()->property("visible").toBool());
}

TEST_F(ProductionQml, ButtonAndWindowCloseKeepEventLoopUntilWorkerIsReleased)
{
    using namespace cadcontour::test;
    ControlledWork work;
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    click("closeButton");
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    EXPECT_TRUE(window->isVisible());
    EXPECT_FALSE(enabled("applyButton"));
    EXPECT_FALSE(enabled("newProjectButton"));
    window->close(); // repeated system-window route is idempotent
    EXPECT_TRUE(window->isVisible());
    bool guiEvent = false;
    QTimer::singleShot(0, [&] { guiEvent = true; });
    ASSERT_TRUE(spinUntil([&] { return guiEvent; }));
    EXPECT_TRUE(window->isVisible());
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    ASSERT_TRUE(spinUntil([&] { return !window->isVisible(); }));
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_EQ(OperationTestAccess::generation(controller), 0);
}

TEST_F(ProductionQml, SystemWindowCloseStartsShutdownWithoutButton)
{
    using namespace cadcontour::test;
    ControlledWork work;
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    window->close();
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    EXPECT_TRUE(window->isVisible());
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    ASSERT_TRUE(spinUntil([&] { return !window->isVisible(); }));
}

TEST_F(ProductionQml, PostedQuitUsesProductionShutdownCoordinator)
{
    using namespace cadcontour::test;
    ControlledWork work;
    auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    ASSERT_NE(app, nullptr);
    ApplicationShutdown coordinator(*app, controller, *window);
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    QEvent quit(QEvent::Quit);
    QCoreApplication::sendEvent(app, &quit);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    EXPECT_TRUE(window->isVisible());
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    ASSERT_TRUE(spinUntil([&] { return !window->isVisible(); }));
}

TEST_F(ProductionQml, EngineAndViewModelCanDisappearBeforeControllerDrain)
{
    using namespace cadcontour::test;
    ControlledWork work;
    OperationTestAccess::start(controller, work.function());
    work.entered.wait();
    // The worker has no engine or view-model references; destroy a separate real presentation.
    {
        ProjectViewModel temporaryViewModel(controller);
        QQmlApplicationEngine temporaryEngine;
        QQmlEngine::setObjectOwnership(&temporaryViewModel, QQmlEngine::CppOwnership);
        temporaryEngine.setInitialProperties({{QStringLiteral("project"), QVariant::fromValue(&temporaryViewModel)}});
        temporaryEngine.loadFromModule("CADContour2D", "Main");
        ASSERT_EQ(temporaryEngine.rootObjects().size(), 1);
    }
    work.complete.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}

TEST(QmlRegistration, ViewModelCannotBeCreatedByQml)
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import CADContour2D\nProjectViewModel {}", QUrl{});
    EXPECT_TRUE(component.isError());
    EXPECT_TRUE(component.errorString().contains(QStringLiteral("supplied by the application")));
}

TEST(QmlRegistration, MainRequiresTypedInjection)
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule("CADContour2D", "Main");
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> missing(component.createWithInitialProperties({}));
    EXPECT_EQ(missing, nullptr);
    EXPECT_TRUE(component.errorString().contains(QStringLiteral("Required property project")));
}

TEST(QmlIntegration, StartupHasNoProjectAndLegacyNewCommandCannotCreateOne)
{
    ProjectController controller;
    ProjectViewModel viewModel(controller);
    QQmlApplicationEngine engine;
    QQmlEngine::setObjectOwnership(&viewModel, QQmlEngine::CppOwnership);
    engine.setInitialProperties({{QStringLiteral("project"), QVariant::fromValue(&viewModel)}});
    engine.loadFromModule("CADContour2D", "Main");
    ASSERT_EQ(engine.rootObjects().size(), 1);
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
    ASSERT_NE(window, nullptr);
    auto *button = window->findChild<QObject *>(QStringLiteral("newProjectButton"));
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(button->property("enabled").toBool());
    EXPECT_FALSE(viewModel.hasProject());
    EXPECT_TRUE(viewModel.cellText().isEmpty());
    viewModel.newProject();
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_FALSE(viewModel.errorMessage().isEmpty());
    window->close();
}
