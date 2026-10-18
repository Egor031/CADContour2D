#include "measurement.h"
#include "ui/project_view_model.h"
#include "ui/application_shutdown.h"
#include "operation_test_access.h"

#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <cstdio>

using namespace cadcontour;

namespace {
QJsonObject project(const ProjectController &controller)
{
    const auto *state = controller.project();
    if (!state) return {{"hasProject", false}};
    const auto *data = state->dataset();
    QJsonObject result{{"hasProject", true}, {"cell", state->cell()},
        {"canUndo", controller.canUndo()}, {"canRedo", controller.canRedo()}};
    number(result, "identity", state->runtimeIdentity());
    if (data) {
        result["cachePath"] = QString::fromStdWString(data->cachePath().wstring());
        result["cacheId"] = QUuid::fromRfc4122(QByteArrayView(
            reinterpret_cast<const char *>(data->cacheId.bytes.data()), 16)).toString(QUuid::WithoutBraces);
        number(result, "sourceBytes", data->sourceBytes);
        number(result, "sourceLines", data->sourceLines);
        number(result, "pointCount", data->pointCount);
        result["hasNonZeroZ"] = data->hasNonZeroZ;
        result["bounds"] = QJsonArray{data->minimum.x, data->minimum.y, data->maximum.x, data->maximum.y};
    }
    return result;
}

void importBlocking(ProjectController &controller, const QString &source)
{
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool timedOut = false;
    const auto connection = QObject::connect(&controller, &ProjectController::operationChanged, &loop, [&] {
        if (!controller.operation().active) loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        controller.cancelOperation();
        if (!controller.operation().active) loop.quit();
    });
    const auto result = controller.importAscXyz({source, {}, controller.project() != nullptr,
                                               controller.importContext(), true});
    require(result.outcome == CommandOutcome::Applied, "setup/retry import rejected");
    deadline.start(600000);
    if (controller.operation().active) loop.exec();
    QObject::disconnect(connection);
    require(!timedOut && controller.operation().status == OperationStatus::Succeeded, "setup/retry import failed");
}
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CADContour2D"));
    QCommandLineParser args;
    args.setApplicationDescription("Opt-in ASC/XYZ acceptance measurements; never part of production CTest.");
    args.addHelpOption();
    args.addOptions({
        {{"s", "source"}, "Source ASC/XYZ.", "path"},
        {"storage", "Explicit storage root; default is production AppLocalDataLocation/projects.", "path"},
        {"report", "New JSON output; also creates report.samples.csv.", "path"},
        {"cache", "Validate an existing accepted cache instead of importing.", "path"},
        {"oracle", "Independent bounded JSON binary64 control records.", "path"},
        {"baseline", "Import this flat source and prepare both Undo/Redo branches before measurement.", "path"},
        {"retry-source", "After headless cancellation, actually import this flat source on the same controller.", "path"},
        {"scenario", "complete, cancel-parsing, cancel-cache, cancel-z, shutdown-parsing, shutdown-cache.", "name", "complete"},
        {"gui", "Load the real Main.qml on the native platform; automate only selection delivery."},
        {"no-validate", "Skip Reader validation for an auxiliary run."}
    });
    if (!args.parse(app.arguments())) {
        std::fprintf(stderr, "%s\n", args.errorText().toUtf8().constData());
        return 2;
    }
    if (args.isSet("help") || args.isSet("help-all")) {
        std::printf("%s", args.helpText().toUtf8().constData());
        return 0;
    }
    const auto reportPath = args.value("report");
    QFile output(reportPath);
    if (reportPath.isEmpty() || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    QJsonObject report{{"tool", "import_acceptance"}, {"qt", qVersion()},
        {"scenario", args.value("scenario")}, {"gui", args.isSet("gui")}};
    int exitCode = 1;
    try {
        const auto storage = args.isSet("storage") ? QFileInfo(args.value("storage")).absoluteFilePath()
            : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/projects";
        report["storageRoot"] = storage;
        const auto scratch = QFileInfo(reportPath).absolutePath() + "/validation-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (args.isSet("cache")) {
            report["validation"] = validateCache(args.value("cache"), scratch, args.value("oracle"));
        } else {
            const auto source = QFileInfo(args.value("source")).absoluteFilePath();
            require(QFileInfo(source).isFile(), "source must be an existing file");
            const auto scenario = args.value("scenario");
            require(scenario == "complete" || scenario == "cancel-parsing" || scenario == "cancel-cache" || scenario == "cancel-z"
                || scenario == "shutdown-parsing" || scenario == "shutdown-cache", "invalid scenario");
            report["sourcePath"] = source;
            number(report, "sourceBytes", std::uint64_t(QFileInfo(source).size()));
            ProjectController controller(storage);
            ProjectViewModel model(controller);
            QQmlEngine::setObjectOwnership(&model, QQmlEngine::CppOwnership);
            if (args.isSet("baseline")) {
                importBlocking(controller, args.value("baseline"));
                require(controller.setCell(2).outcome == CommandOutcome::Applied
                    && controller.setCell(3).outcome == CommandOutcome::Applied
                    && controller.undo().outcome == CommandOutcome::Applied
                    && controller.canUndo() && controller.canRedo(), "baseline history preparation failed");
            }
            report["beforeProject"] = project(controller);
            const auto oldIdentity = controller.project() ? controller.project()->runtimeIdentity() : 0;
            const auto start = std::chrono::steady_clock::now();
            std::atomic<double> readyMs{-1}, decisionMs{-1}, cleanupMs{-1};
            OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false,
                [&] { readyMs = since(start); },
                [&](PublicationDecision) { cleanupMs = since(start); });
            OperationRunnerTestAccess::beforeOwnershipFinalization(OperationTestAccess::runner(controller),
                [&](PublicationDecision) { decisionMs = since(start); });
            Measurement monitor(storage, reportPath + ".samples.csv");
            QJsonArray phases, warnings, timerOutliers;
            double launchMs = -1, acceptMs = -1, completeMs = -1, cancellationMs = -1;
            double maxTickGap = 0, previousTick = since(start);
            int lastPhase = -1, guiObjects = 0, initialGuiObjects = 0;
            std::uint64_t ticks = 0, gapsOver50 = 0, gapsOver100 = 0;
            bool actionQueued = false, busyQueued = false, busyChecked = false;
            bool busyRejected = false, canImportAtCompletion = false;
            QObject callbacks; // Disconnect before captured measurements and adapter/controller destruction.
            QTimer heartbeat;
            heartbeat.setTimerType(Qt::PreciseTimer);
            heartbeat.setInterval(10);
            QObject::connect(&heartbeat, &QTimer::timeout, &callbacks, [&] {
                const auto now = since(start);
                const auto gap = now - previousTick;
                previousTick = now;
                maxTickGap = std::max(maxTickGap, gap);
                ++ticks;
                gapsOver50 += gap > 50; gapsOver100 += gap > 100;
                if (gap > 50 && timerOutliers.size() < 256)
                    timerOutliers.append(QJsonObject{{"observedMs", now}, {"gapMs", gap},
                        {"phase", int(controller.operation().progress.phase)},
                        {"operationActive", controller.operation().active}});
                if (scenario == "cancel-z" && !actionQueued && controller.pendingNonZeroZ()) {
                    const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
                    bool waiting = false;
                    if (context) {
                        std::lock_guard lock(context->mutex);
                        waiting = context->waitingForZ;
                        if (waiting) report["workerPhaseAtCancellation"] = int(context->progress.phase);
                    }
                    if (waiting) {
                        actionQueued = true;
                        report["workerWaitingForZAtCancellation"] = true;
                        number(report, "zPhysicalLine", controller.pendingNonZeroZ()->line);
                        cancellationMs = since(start);
                        model.answerNonZeroZ(model.nonZeroZToken(), false);
                    }
                }
            });
            QObject::connect(&controller, &ProjectController::stateChanged, &callbacks, [&] {
                if (controller.project() && controller.project()->runtimeIdentity() != oldIdentity)
                    acceptMs = since(start);
            });
            QObject::connect(&controller, &ProjectController::operationChanged, &callbacks, [&] {
                const auto &operation = controller.operation();
                const auto phase = operation.progress.phase;
                if (operation.active && int(phase) != lastPhase) {
                    lastPhase = int(phase);
                    phases.append(QJsonObject{{"phase", lastPhase}, {"observedMs", since(start)}});
                    std::printf("phase=%d ms=%.3f\n", lastPhase, since(start)); std::fflush(stdout);
                }
                if (operation.active && phase == OperationPhase::Parsing && !busyQueued) {
                    busyQueued = true;
                    QTimer::singleShot(0, &callbacks, [&] {
                        if (!controller.operation().active) return;
                        busyChecked = true;
                        const auto result = controller.importAscXyz({source, {}, true, {}, true});
                        busyRejected = result.outcome == CommandOutcome::Rejected
                            && result.failure->category == CommandError::Busy && !model.canImport();
                    });
                }
                const auto desired = scenario.endsWith("cache") ? OperationPhase::BuildingCache : OperationPhase::Parsing;
                if (scenario != "complete" && scenario != "cancel-z" && operation.active && phase == desired && !actionQueued) {
                    actionQueued = true;
                    QTimer::singleShot(0, &callbacks, [&] {
                        const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
                        if (context) {
                            std::lock_guard lock(context->mutex);
                            report["workerPhaseAtCancellation"] = int(context->progress.phase);
                        }
                        cancellationMs = since(start);
                        if (scenario.startsWith("shutdown")) model.requestClose();
                        else model.cancelOperation();
                    });
                }
                if (operation.id && !operation.active) {
                    completeMs = since(start);
                    canImportAtCompletion = model.canImport();
                    QTimer::singleShot(0, &app, &QCoreApplication::quit);
                }
            });
            QObject::connect(&model, &ProjectViewModel::fileSelectionRequested, &callbacks, [&] {
                const auto token = model.selectionToken();
                QTimer::singleShot(0, &callbacks, [&, token] {
                    launchMs = since(start);
                    model.selectImportFile(token, QUrl::fromLocalFile(source));
                    if (model.replacementPending()) model.confirmReplacement(token, true);
                });
            });
            {
                QQmlApplicationEngine engine;
                QTimer countObjects;
                QQuickWindow *window = nullptr;
                std::unique_ptr<ApplicationShutdown> shutdown;
                if (args.isSet("gui")) {
                    QObject::connect(&engine, &QQmlEngine::warnings, &callbacks, [&](const auto &errors) {
                        for (const auto &error : errors) warnings.append(error.toString());
                    });
                    engine.setInitialProperties({{"project", QVariant::fromValue(&model)}, {"fileDialogEnabled", false}});
                    engine.loadFromModule("CADContour2D", "Main");
                    require(engine.rootObjects().size() == 1, "Main.qml failed");
                    window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
                    require(window != nullptr, "Main.qml root is not a window");
                    initialGuiObjects = int(window->findChildren<QObject *>().size());
                    guiObjects = initialGuiObjects;
                    QObject::connect(&countObjects, &QTimer::timeout, &engine, [&] {
                        guiObjects = std::max(guiObjects, int(window->findChildren<QObject *>().size()));
                    });
                    countObjects.start(100);
                    shutdown = std::make_unique<ApplicationShutdown>(app, controller, *window);
                }
                if (!args.isSet("gui") || controller.project())
                    QTimer::singleShot(0, &model, &ProjectViewModel::newProject);
                previousTick = since(start);
                heartbeat.start();
                app.exec();
                heartbeat.stop();
            }
            const auto importResources = monitor.json();
            report["importResources"] = importResources;
            report["phaseTransitions"] = phases;
            report["warnings"] = warnings;
            report["guiTimerOutliersOver50Ms"] = timerOutliers;
            report["importStartRequestedMs"] = launchMs;
            report["importToNativeCompletionMs"] = completeMs - launchMs;
            report["readyPublishedWorkerHookMs"] = readyMs.load();
            report["applicationAcceptObservedMs"] = acceptMs;
            report["workerDecisionObservedMs"] = decisionMs.load();
            report["workerProductCleanupHookMs"] = cleanupMs.load();
            report["nativeCompletionJoinedObservedMs"] = completeMs;
            report["cancellationRequestedMs"] = cancellationMs;
            if (cancellationMs >= 0) report["cancelToNativeCompletionMs"] = completeMs - cancellationMs;
            report["canImportAtCompletion"] = canImportAtCompletion;
            report["secondImportProbeExecuted"] = busyChecked;
            report["secondImportRejectedBusy"] = busyRejected;
            report["maxGuiTimerGapMs"] = maxTickGap;
            number(report, "guiTimerTicks", ticks);
            number(report, "guiTimerGapsOver50Ms", gapsOver50);
            number(report, "guiTimerGapsOver100Ms", gapsOver100);
            report["initialWindowDescendantObjects"] = initialGuiObjects;
            report["sampledPeakWindowDescendantObjects"] = guiObjects;
            report["afterProject"] = project(controller);
            report["operationStatus"] = int(controller.operation().status);
            report["publicationDecision"] = int(controller.operation().decision);
            report["cleanupWarning"] = controller.operation().cleanupWarning;
            if (controller.operation().failure)
                report["diagnostic"] = model.importDiagnostic().isEmpty() ? QJsonObject{}
                    : QJsonObject::fromVariantMap(model.importDiagnostic());
            require(completeMs >= 0 && !controller.operation().active && warnings.isEmpty(), "incomplete operation or QML warning");
            require(!busyChecked || busyRejected, "second import was not rejected Busy");
            require(!importResources["nativeApiOrSampleFailure"].toBool(), "measurement failed");
            if (scenario == "complete") {
                require(controller.operation().status == OperationStatus::Succeeded
                    && controller.operation().decision == PublicationDecision::Accept, "import not accepted");
                require(controller.project()->cell() == 1 && !controller.canUndo() && !controller.canRedo(), "new project/history mismatch");
                if (!args.isSet("no-validate")) {
                    const auto path = QString::fromStdWString(controller.project()->dataset()->cachePath().wstring());
                    report["validation"] = validateCache(path, scratch, args.value("oracle"));
                    require(report["validation"].toObject()["cacheId"] == report["afterProject"].toObject()["cacheId"],
                        "accepted/Reader CacheId mismatch");
                }
            } else {
                require(actionQueued && controller.operation().status == OperationStatus::Cancelled
                    && controller.operation().decision == PublicationDecision::Discard, "cancellation failed");
                require(report["beforeProject"] == report["afterProject"], "cancel/shutdown changed the previous project/history");
                require(importResources["remainingNewFiles"].toString() == "0", "unaccepted files retained");
                if (args.isSet("retry-source")) {
                    require(!scenario.startsWith("shutdown") && !args.isSet("gui"), "retry requires open headless controller");
                    importBlocking(controller, args.value("retry-source"));
                    report["retryProject"] = project(controller);
                    report["retrySucceeded"] = true;
                }
            }
        }
        report["status"] = "PASS";
        exitCode = 0;
    } catch (const std::exception &error) {
        report["status"] = "FAIL";
        report["error"] = QString::fromUtf8(error.what());
        std::fprintf(stderr, "%s\n", error.what());
    }
    const auto json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (output.write(json) != json.size() || !output.flush()) return 3;
    return exitCode;
}
