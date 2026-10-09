#include "application/project_controller.h"
#include "ui/project_view_model.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml/qqml.h>

#include <cstdlib>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CADContour2D"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    cadcontour::ProjectController controller;
    if (controller.newProject().outcome != cadcontour::CommandOutcome::Applied)
        return EXIT_FAILURE;
    cadcontour::ProjectViewModel viewModel(controller);
    QQmlEngine::setObjectOwnership(&viewModel, QQmlEngine::CppOwnership);
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("project"), QVariant::fromValue(&viewModel)}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("CADContour2D", "Main");

    return app.exec();
}
