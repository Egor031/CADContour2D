#include "project_test_access.h"
#include "operation_probe.h"
#include "ui/application_shutdown.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QTimer>

#include <cstdlib>
#include <iostream>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CADContour2D-operation-probe"));
    cadcontour::ProjectController controller;
    cadcontour::ProjectControllerTestAccess::newProject(controller);
    cadcontour::ProjectViewModel viewModel(controller);
    cadcontour::OperationProbe probe(controller);
    QQmlEngine::setObjectOwnership(&viewModel, QQmlEngine::CppOwnership);
    QQmlEngine::setObjectOwnership(&probe, QQmlEngine::CppOwnership);
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("project"), QVariant::fromValue(&viewModel)},
                                 {QStringLiteral("probe"), QVariant::fromValue(&probe)}});
    engine.loadFromModule("CADContour2D.Verification", "ProbeWindow");
    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;
    auto *window = qobject_cast<QWindow *>(engine.rootObjects().front());
    if (!window)
        return EXIT_FAILURE;
    cadcontour::ApplicationShutdown shutdown(app, controller, *window);
    const auto arguments = app.arguments();
    const int autoIndex = arguments.indexOf(QStringLiteral("--auto"));
    const auto scenario = autoIndex >= 0 ? arguments.value(autoIndex + 1) : QString{};
    if (!scenario.isEmpty()) {
        QObject::connect(&viewModel, &cadcontour::ProjectViewModel::workflowChanged, &app, [&] {
            if (viewModel.workflowPrompt() == QStringLiteral("dirty")) {
                const auto token = viewModel.workflowToken();
                QTimer::singleShot(0, &viewModel, [&, token] { viewModel.answerWorkflow(token, QStringLiteral("discard")); });
            }
        });
        QTimer::singleShot(0, &probe, [&] {
            if (scenario == QStringLiteral("unknown")) probe.startUnknown();
            else if (scenario == QStringLiteral("failure")) probe.startFailure();
            else probe.startKnown();
        });
        QObject::connect(&controller, &cadcontour::ProjectController::operationChanged, &app, [&] {
            if (!controller.operation().active)
                QTimer::singleShot(0, window, &QWindow::close);
        });
        if (scenario == QStringLiteral("close"))
            QTimer::singleShot(300, window, &QWindow::close);
        else if (scenario == QStringLiteral("quit"))
            QTimer::singleShot(300, &app, &QCoreApplication::quit);
        else if (scenario == QStringLiteral("cancel"))
            QTimer::singleShot(300, &viewModel, &cadcontour::ProjectViewModel::cancelOperation);
        else if (scenario == QStringLiteral("replace"))
            QTimer::singleShot(300, &controller, [&] { cadcontour::ProjectControllerTestAccess::newProject(controller); });
    }
    const auto result = app.exec();
    if (!scenario.isEmpty()) {
        std::cout << "scenario=" << scenario.toStdString() << " status=" << int(controller.operation().status)
                  << " decision=" << int(controller.operation().decision)
                  << " shutdown=" << int(controller.shutdownState()) << '\n';
        const bool successExpected = scenario == QStringLiteral("known") || scenario == QStringLiteral("unknown");
        const bool correct = !controller.operation().active
            && controller.shutdownState() == cadcontour::ShutdownState::Ready
            && (successExpected ? controller.operation().status == cadcontour::OperationStatus::Succeeded
                : scenario == QStringLiteral("failure") ? controller.operation().status == cadcontour::OperationStatus::Failed
                : scenario == QStringLiteral("replace") ? controller.operation().decision == cadcontour::PublicationDecision::Discard
                : controller.operation().status == cadcontour::OperationStatus::Cancelled);
        return correct ? result : EXIT_FAILURE;
    }
    return result;
}
