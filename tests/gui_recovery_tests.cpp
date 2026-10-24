#include "ui/project_view_model.h"
#include "ui/application_shutdown.h"
#include "operation_test_support.h"
#include "recovery_test_access.h"
#include "save_test_access.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQmlApplicationEngine>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <tuple>

using namespace cadcontour;
using namespace cadcontour::test;
using namespace cadcontour::persistence;

namespace {
QByteArray bytes(const QString &path)
{ QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
void write(const QString &path, const QByteArray &data)
{ QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write(data), data.size()); }

class RecoveryGui : public testing::Test {
protected:
    QTemporaryDir area;
    QString storage = area.filePath("imports");
    ProjectController controller{storage};
    ProjectViewModel model{controller};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    std::vector<QString> warnings;

    void settle(ProjectController &owner)
    {
        ASSERT_TRUE(spinUntil([&] {
            OperationTestAccess::poll(owner);
            return RecoveryTestAccess::settled(owner);
        }));
        RecoveryTestAccess::timers(owner);
    }
    void idle()
    {
        ASSERT_TRUE(spinUntil([&] {
            OperationTestAccess::poll(controller);
            return RecoveryTestAccess::settled(controller)
                && (!model.workflowPending() || !model.workflowPrompt().isEmpty());
        }));
        RecoveryTestAccess::timers(controller);
        QCoreApplication::processEvents();
    }
    void load()
    {
        ASSERT_TRUE(area.isValid());
        QQmlEngine::setObjectOwnership(&model, QQmlEngine::CppOwnership);
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.push_back(error.toString());
        });
        engine.setInitialProperties({{"project", QVariant::fromValue(&model)}, {"fileDialogEnabled", false}});
        engine.loadFromModule("CADContour2D", "Main");
        ASSERT_EQ(engine.rootObjects().size(), 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front()); ASSERT_TRUE(window);
        ASSERT_TRUE(spinUntil([&] { return model.hasProject() || model.selectingFile() || model.workflowPrompt() == "recovery"; }));
        idle();
    }
    void TearDown() override
    {
        controller.cancelOperation(); settle(controller);
        for (const auto &warning : warnings) ADD_FAILURE() << warning.toStdString();
    }
    QString source(const char *name = "source.xyz")
    { const auto path = area.filePath(name); write(path, "1 2 0\n3 4 0\n"); return path; }
    void import(ProjectController &owner, const QString &path)
    {
        ASSERT_EQ(owner.importAscXyz({path, {8ULL << 20, 64ULL << 20, 8}, bool(owner.project())}).outcome, CommandOutcome::Applied);
        settle(owner); ASSERT_TRUE(owner.project());
    }
    void autosave(ProjectController &owner)
    {
        // An immediate post-Save tick may already have published this exact content.
        const auto result = owner.autosave();
        ASSERT_NE(result.outcome, CommandOutcome::Rejected);
        settle(owner); ASSERT_TRUE(owner.recoveryRecord());
        EXPECT_TRUE(matchesSavedContent(*owner.project(), *owner.recoveryRecord()->state));
    }
    RecoveryCandidate seed(double cell = .125, bool saved = false)
    {
        ProjectController producer(storage);
        import(producer, source());
        if (saved) {
            EXPECT_EQ(producer.saveProject({area.filePath("saved/project.pc2dproj")}).outcome, CommandOutcome::Applied);
            settle(producer); EXPECT_TRUE(producer.savedProject());
        }
        EXPECT_NE(producer.setCell(cell).outcome, CommandOutcome::Rejected); autosave(producer);
        const auto id = producer.recoveryRecord()->sessionId;
        auto found = discoverRecovery(RecoveryTestAccess::root(producer)); EXPECT_TRUE(found);
        if (found) for (const auto &candidate : found.value()) if (candidate.sessionId == id) return candidate;
        ADD_FAILURE() << "seed candidate missing"; return {};
    }
    QString recordPath(const RecoveryCandidate &c)
    { return QDir(RecoveryTestAccess::root(controller)).filePath(recoveryFileName(c.sessionId)); }
    QObject *control(const char *name)
    {
        const auto target = QString::fromLatin1(name);
        auto *object = window->findChild<QObject *>(target);
        const auto visual = [&](auto &&self, QQuickItem *item) -> QObject * {
            if (item->objectName() == target) return item;
            for (auto *child : item->childItems()) if (auto *found = self(self, child)) return found;
            return nullptr;
        };
        if (!object) object = visual(visual, window->contentItem());
        EXPECT_TRUE(object) << name; return object;
    }
    void invoke(const char *name, const char *method)
    { ASSERT_TRUE(QMetaObject::invokeMethod(control(name), method, Qt::DirectConnection)); }
    void click(const char *name) { invoke(name, "clicked"); }
    void answer(const char *value) { model.answerWorkflow(model.workflowToken(), QString::fromLatin1(value)); }
    void choose(const QString &path)
    {
        ASSERT_TRUE(control("projectFileDialog")->setProperty("selectedFile", QUrl::fromLocalFile(path)));
        invoke("projectFileDialog", "accepted"); idle();
    }
    void select(const RecoveryCandidate &candidate, const char *action = "restore")
    { model.chooseRecovery(model.workflowToken(), candidate.sessionId.toString(QUuid::WithoutBraces), QString::fromLatin1(action)); }
    QVariantMap row(const RecoveryCandidate &candidate)
    {
        for (const auto &value : model.recoveryCandidates()) {
            auto result = value.toMap();
            if (result["sessionId"].toString() == candidate.sessionId.toString(QUuid::WithoutBraces)) return result;
        }
        ADD_FAILURE() << "missing row"; return {};
    }
    void rediscover()
    { answer("refresh"); idle(); ASSERT_EQ(model.workflowPrompt(), "recovery"); }
};

TEST_F(RecoveryGui, EmptyStartupOffersImportOnlyAfterMetadataDiscovery)
{
    load(); EXPECT_TRUE(model.selectingFile()); EXPECT_FALSE(model.workflowPending());
    EXPECT_EQ(controller.operation().kind, OperationKind::DiscoverRecovery);
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_FALSE(control("recoveryDialog")->property("visible").toBool());
    invoke("importFileDialog", "rejected"); model.offerStartupImport();
    EXPECT_FALSE(model.selectingFile()); EXPECT_FALSE(model.hasProject());
}

class DeferredRecovery : public RecoveryGui, public testing::WithParamInterface<int> {};
TEST_P(DeferredRecovery, CloseEscapeAndLaterNeverRejectAndMenuCanRestore)
{
    const auto candidate = seed(); const auto before = bytes(recordPath(candidate)); load();
    ASSERT_EQ(model.workflowPrompt(), "recovery"); EXPECT_FALSE(model.selectingFile());
    EXPECT_TRUE(control("recoveryDialog")->property("visible").toBool());
    EXPECT_TRUE(control("importFileDialog")->property("requestToken").toString().isEmpty());
    if (GetParam() == 0) click("deferRecoveryButton");
    else if (GetParam() == 1) invoke("recoveryDialog", "close");
    else answer("cancel"); // same tokenized Escape/closed cancellation contract
    ASSERT_TRUE(spinUntil([&] { return model.selectingFile(); }));
    EXPECT_EQ(bytes(recordPath(candidate)), before);
    invoke("importFileDialog", "rejected"); invoke("recoveryAction", "triggered"); idle();
    EXPECT_TRUE(row(candidate)["eligible"].toBool());
    click("restoreRecoveryButton0"); idle();
    ASSERT_TRUE(model.hasProject()); EXPECT_DOUBLE_EQ(model.cell(), .125);
    EXPECT_EQ(model.cellDraft(), "0.125"); EXPECT_TRUE(model.dirty()); EXPECT_TRUE(model.projectPath().isEmpty());
    EXPECT_FALSE(model.canUndo()); EXPECT_FALSE(model.canRedo()); EXPECT_TRUE(model.cacheAvailable());
    EXPECT_EQ(controller.pointCacheAvailability()->reads.readBytes, 73728);
    EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0);
    EXPECT_FALSE(model.selectingFile()); EXPECT_FALSE(model.workflowPending());
    EXPECT_TRUE(model.fileMessage().contains(QStringLiteral("восстановлен")));
    EXPECT_EQ(controller.project()->dataset()->cachePath(), candidate.record->state->content.cachePath());
}
INSTANTIATE_TEST_SUITE_P(DeferredDecisions, DeferredRecovery, testing::Range(0, 3));

TEST_F(RecoveryGui, MultipleCandidatesRejectIsExplicitAndDoesNotDeleteDatasets)
{
    const auto a = seed(.125), b = seed(.25); load(); ASSERT_EQ(model.recoveryCandidates().size(), 2);
    const auto before = bytes(recordPath(a)); select(a, "reject"); ASSERT_EQ(model.workflowPrompt(), "rejectRecovery");
    click("cancelRecoveryButton"); QCoreApplication::processEvents();
    EXPECT_EQ(bytes(recordPath(a)), before);
    // Startup continues only after the explicit cancellation, never concurrently with the recovery dialog.
    if (model.selectingFile()) invoke("importFileDialog", "rejected");
    model.showRecovery(); idle(); select(a, "reject"); click("confirmRecoveryButton"); idle();
    EXPECT_FALSE(row(a)["eligible"].toBool()); EXPECT_TRUE(row(a)["description"].toString().contains("Rejected"));
    EXPECT_TRUE(row(b)["eligible"].toBool());
    EXPECT_TRUE(QFile::exists(QString::fromStdWString(a.record->state->content.cachePath().wstring())));
    ProjectController restarted(storage); ASSERT_EQ(restarted.discoverRecovery().outcome, CommandOutcome::Applied); settle(restarted);
    bool rejected = false;
    for (const auto &c : restarted.recoveryCandidates()) if (c.sessionId == a.sessionId) rejected = c.session == RecoverySessionState::Rejected;
    EXPECT_TRUE(rejected); EXPECT_FALSE(controller.project());
}

TEST_F(RecoveryGui, ActiveInvalidAndUnsupportedHaveDiagnosticsAndCppGuards)
{
    auto active = std::make_unique<ProjectController>(storage); import(*active, source()); autosave(*active);
    auto found = discoverRecovery(RecoveryTestAccess::root(controller)); ASSERT_TRUE(found); ASSERT_EQ(found.value().size(), 1);
    const auto live = found.value()[0];
    const auto corrupt = QUuid::createUuid(), unsupported = QUuid::createUuid();
    write(QDir(RecoveryTestAccess::root(controller)).filePath(recoveryFileName(corrupt)), "broken");
    write(QDir(RecoveryTestAccess::root(controller)).filePath(recoveryFileName(unsupported)), "PC2D-RECOVERY 999\n");
    load(); ASSERT_EQ(model.recoveryCandidates().size(), 3);
    EXPECT_FALSE(row(live)["eligible"].toBool()); EXPECT_TRUE(row(live)["description"].toString().contains("Active"));
    const auto operation = controller.operation().id; select(live); EXPECT_EQ(controller.operation().id, operation);
    EXPECT_FALSE(persistence::restoreRecovery(RecoveryTestAccess::root(controller), live, {}, {1ULL << 20, 0, 2}));
    EXPECT_FALSE(persistence::rejectRecovery(RecoveryTestAccess::root(controller), live));
    for (const auto &value : model.recoveryCandidates()) {
        const auto r = value.toMap(); EXPECT_FALSE(r["eligible"].toBool());
        if (r["sessionId"].toString() == unsupported.toString(QUuid::WithoutBraces))
            EXPECT_TRUE(r["description"].toString().contains("Unsupported recovery"));
    }
    active.reset(); rediscover(); EXPECT_TRUE(row(live)["eligible"].toBool());
}

TEST_F(RecoveryGui, DiscoveryFailureRequiresDecisionAndCanBeRetried)
{
    ASSERT_TRUE(QDir().mkpath(storage)); write(RecoveryTestAccess::root(controller), "not a directory"); load();
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_FALSE(model.selectingFile());
    EXPECT_TRUE(control("recoveryMessageLabel")->property("text").toString().contains("Recovery:"));
    ASSERT_TRUE(QFile::remove(RecoveryTestAccess::root(controller)));
    answer("refresh"); idle(); ASSERT_TRUE(spinUntil([&] { return model.selectingFile(); }));
    EXPECT_FALSE(control("recoveryDialog")->property("visible").toBool());
}

class ExplicitRecovery : public RecoveryGui, public testing::WithParamInterface<bool> {};
TEST_P(ExplicitRecovery, MatchingAndModifiedExplicitStateRestoreCorrectBaseline)
{
    const auto candidate = seed(GetParam() ? .125 : 1, true); load();
    const auto text = row(candidate)["description"].toString();
    EXPECT_TRUE(text.contains(GetParam() ? "Modified" : "MatchesExplicit"));
    select(candidate); idle(); EXPECT_EQ(model.dirty(), GetParam());
    EXPECT_EQ(model.projectPath(), area.filePath("saved/project.pc2dproj")); EXPECT_FALSE(model.recoveryRequiresSaveAs());
    EXPECT_FALSE(model.canUndo()); EXPECT_FALSE(model.canRedo());
}
INSTANTIATE_TEST_SUITE_P(Baselines, ExplicitRecovery, testing::Bool());

TEST_F(RecoveryGui, ConflictRequiresSaveAsThroughFailureCancelAutosaveAndSuccess)
{
    const auto candidate = seed(.125, true); const auto path = area.filePath("saved/project.pc2dproj");
    {
        ProjectController other(area.filePath("other"));
        ASSERT_EQ(other.openProject({path}).outcome, CommandOutcome::Applied); settle(other);
        other.setCell(.25); ASSERT_EQ(other.saveProject().outcome, CommandOutcome::Applied); settle(other);
    }
    const auto explicitBytes = bytes(path); load(); EXPECT_TRUE(row(candidate)["description"].toString().contains("Conflict"));
    select(candidate); idle(); ASSERT_TRUE(model.recoveryRequiresSaveAs()); EXPECT_TRUE(model.dirty());
    EXPECT_TRUE(control("recoveryConflictLabel")->property("visible").toBool());
    EXPECT_EQ(controller.saveProject().outcome, CommandOutcome::Rejected);
    model.save(); ASSERT_EQ(model.workflowPrompt(), "save"); answer("cancel");
    EXPECT_TRUE(model.recoveryRequiresSaveAs());
    SaveTestAccess::configure(controller, {}, [](QIODevice &, const char *, qint64) -> qint64 { return -1; });
    model.save(); choose(area.filePath("failed/project.pc2dproj"));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_TRUE(model.recoveryRequiresSaveAs());
    SaveTestAccess::configure(controller, {}); controller.setCell(.0625); autosave(controller);
    EXPECT_TRUE(controller.recoveryRecord()->requiresSaveAs); EXPECT_TRUE(model.recoveryRequiresSaveAs());
    model.save(); choose(area.filePath("resolved/project.pc2dproj"));
    EXPECT_FALSE(model.recoveryRequiresSaveAs()); EXPECT_FALSE(model.dirty()); EXPECT_EQ(bytes(path), explicitBytes);
    EXPECT_EQ(model.projectPath(), area.filePath("resolved/project.pc2dproj"));
    EXPECT_EQ(controller.project()->dataset()->location, controller.savedProject()->content.location);
    controller.setCell(.5); model.save(); idle(); EXPECT_FALSE(model.dirty());
    EXPECT_EQ(bytes(path), explicitBytes);
}

class PartialRecovery : public RecoveryGui, public testing::WithParamInterface<int> {};
TEST_P(PartialRecovery, UnavailableCacheRestoresMetadataAndOnlyExplicitRecheckEnablesCommands)
{
    const auto candidate = seed(); const auto cache = QString::fromStdWString(candidate.record->state->content.cachePath().wstring());
    ASSERT_TRUE(QFile::rename(cache, cache + ".held"));
    if (GetParam() == 1) write(cache, "truncated cache");
    if (GetParam() == 2) {
        auto data = bytes(cache + ".held"); data.replace(8, 4, QByteArray::fromHex("02000000"));
        data.replace(2016, 32, QCryptographicHash::hash(data.first(2016), QCryptographicHash::Sha256)); write(cache, data);
    }
    load(); select(candidate); idle();
    ASSERT_TRUE(model.hasProject()); EXPECT_DOUBLE_EQ(model.cell(), .125); EXPECT_EQ(model.pointCountText(), "2");
    EXPECT_FALSE(model.cacheAvailable()); EXPECT_FALSE(model.canSave()); EXPECT_FALSE(model.canEditCell()); EXPECT_TRUE(model.canRecheck());
    EXPECT_EQ(controller.pointCacheAvailability()->access, GetParam() == 0 ? CacheAccess::Missing
        : GetParam() == 1 ? CacheAccess::Corrupt : CacheAccess::Unsupported);
    EXPECT_FALSE(control("cacheStatusLabel")->property("text").toString().isEmpty());
    EXPECT_EQ(controller.setCell(2).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.saveProject({area.filePath("blocked/project.pc2dproj")}).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Rejected);
    if (GetParam() != 0) ASSERT_TRUE(QFile::remove(cache));
    ASSERT_TRUE(QFile::rename(cache + ".held", cache)); EXPECT_FALSE(model.cacheAvailable());
    click("recheckCacheButton"); idle(); EXPECT_TRUE(model.cacheAvailable()); EXPECT_TRUE(model.canSave());
    EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0); EXPECT_EQ(controller.pointCacheAvailability()->reads.readBytes, 73728);
    EXPECT_FALSE(model.canUndo()); EXPECT_FALSE(model.canRedo());
}
INSTANTIATE_TEST_SUITE_P(MissingCorruptUnsupported, PartialRecovery, testing::Range(0, 3));

class RecoveryReplacement : public RecoveryGui, public testing::WithParamInterface<int> {};
TEST_P(RecoveryReplacement, ExistingDraftDirtyReplacementUsesTheFileWorkflow)
{
    const auto candidate = seed(); import(controller, source("current.xyz")); controller.setCell(2);
    // Startup must not offer Import when a project is already open.
    load(); control("cellInput")->setProperty("text", "3");
    const auto identity = model.runtimeIdentity(); model.showRecovery(); idle(); select(candidate);
    ASSERT_EQ(model.workflowPrompt(), "replaceRecovery"); const auto stale = model.workflowToken();
    if (GetParam() == 0) { answer("cancel"); EXPECT_EQ(model.runtimeIdentity(), identity); EXPECT_TRUE(model.hasDraft()); return; }
    answer("confirm"); ASSERT_EQ(model.workflowPrompt(), "draft");
    if (GetParam() == 1) { answer("cancel"); EXPECT_EQ(model.runtimeIdentity(), identity); EXPECT_TRUE(model.hasDraft()); return; }
    answer("apply"); ASSERT_EQ(model.workflowPrompt(), "dirty");
    if (GetParam() == 2) { answer("cancel"); EXPECT_DOUBLE_EQ(model.cell(), 3); EXPECT_EQ(model.runtimeIdentity(), identity); return; }
    if (GetParam() == 3) { answer("save"); choose(area.filePath("current/project.pc2dproj")); }
    else answer("discard");
    idle(); EXPECT_NE(model.runtimeIdentity(), identity); EXPECT_DOUBLE_EQ(model.cell(), .125);
    EXPECT_FALSE(model.canUndo()); EXPECT_FALSE(model.canRedo());
    model.answerWorkflow(stale, "confirm"); EXPECT_FALSE(model.workflowPending());
    if (GetParam() == 3) { QFile file(area.filePath("current/project.pc2dproj")); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        auto saved = readManifest(file, {4ULL << 20, 4096, 32}); ASSERT_TRUE(saved) << saved.error().detail.toStdString();
        EXPECT_DOUBLE_EQ(saved.value().cell, 3); }
}
INSTANTIATE_TEST_SUITE_P(Decisions, RecoveryReplacement, testing::Range(0, 5));

TEST_F(RecoveryGui, StaleTokensAndChangedRecordNeverReplaceOrRejectOtherState)
{
    const auto candidate = seed(); load(); const auto stale = model.workflowToken(); rediscover();
    model.chooseRecovery(stale, candidate.sessionId.toString(QUuid::WithoutBraces), "reject");
    EXPECT_EQ(model.workflowPrompt(), "recovery");
    const auto old = bytes(recordPath(candidate)); write(recordPath(candidate), "changed externally");
    select(candidate); idle(); EXPECT_FALSE(model.hasProject()); EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(model.workflowPrompt(), "recovery"); EXPECT_TRUE(model.fileMessage().contains("Recovery:"));
    EXPECT_EQ(bytes(recordPath(candidate)), "changed externally"); write(recordPath(candidate), old);
    rediscover(); select(candidate, "reject"); const auto confirmation = model.workflowToken();
    import(controller, source("different.xyz"));
    model.answerWorkflow(confirmation, "confirm"); idle();
    EXPECT_TRUE(model.hasProject()); EXPECT_DOUBLE_EQ(model.cell(), 1); EXPECT_EQ(bytes(recordPath(candidate)), old);
}

TEST_F(RecoveryGui, RejectWriteFailurePreservesRecordAndCurrentProject)
{
    const auto candidate = seed(); const auto before = bytes(recordPath(candidate)); load();
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->write = [](QIODevice &, const char *, qint64) -> qint64 { return -1; };
    RecoveryTestAccess::configure(controller, hooks); select(candidate, "reject"); answer("confirm"); idle();
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_EQ(bytes(recordPath(candidate)), before);
    EXPECT_FALSE(model.hasProject()); EXPECT_EQ(model.workflowPrompt(), "recovery");
    EXPECT_TRUE(model.fileMessage().contains("Recovery:"));
}

class RecoveryCrashGui : public RecoveryGui, public testing::WithParamInterface<std::tuple<QString, bool>> {};
TEST_P(RecoveryCrashGui, RealProcessCrashIsDiscoveredAndRestoredOrRejectedInProductionQml)
{
    const auto &[mode, reject] = GetParam();
    const auto marker = area.filePath("ready"); QProcess child;
    child.start(QStringLiteral(RECOVERY_CRASH_PROBE_EXE), {area.path(), mode, marker}); ASSERT_TRUE(child.waitForStarted());
    ASSERT_TRUE(spinUntil([&] { return QFile::exists(marker) || child.state() == QProcess::NotRunning; }));
    ASSERT_TRUE(QFile::exists(marker)) << child.readAllStandardError().constData();
    child.kill(); ASSERT_TRUE(child.waitForFinished(10000));
    ASSERT_TRUE(QFile::remove(area.filePath("source.xyz"))); // restore never reads the source
    load(); ASSERT_EQ(model.recoveryCandidates().size(), 1);
    EXPECT_TRUE(model.recoveryCandidates()[0].toMap()["description"].toString().contains("Interrupted"));
    if (reject) {
        click("rejectRecoveryButton0"); click("confirmRecoveryButton"); idle();
        EXPECT_FALSE(model.hasProject()); EXPECT_FALSE(model.recoveryCandidates()[0].toMap()["eligible"].toBool());
        ProjectController next(storage); ASSERT_EQ(next.discoverRecovery().outcome, CommandOutcome::Applied); settle(next);
        ASSERT_EQ(next.recoveryCandidates().size(), 1); EXPECT_EQ(next.recoveryCandidates()[0].session, RecoverySessionState::Rejected);
        return;
    }
    click("restoreRecoveryButton0"); idle(); ASSERT_TRUE(model.hasProject()); EXPECT_TRUE(model.cacheAvailable());
    EXPECT_DOUBLE_EQ(model.cell(), mode == "published" || mode == "explicit" ? .125 : 1);
    EXPECT_EQ(model.projectPath().isEmpty(), mode != "explicit");
    EXPECT_TRUE(model.dirty()); EXPECT_FALSE(model.canUndo());
    EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0);
}
INSTANTIATE_TEST_SUITE_P(ProcessTermination, RecoveryCrashGui,
    testing::Combine(testing::Values(QStringLiteral("active"), QStringLiteral("published"), QStringLiteral("explicit")), testing::Bool()));

class RecoveryAdmission : public RecoveryGui, public testing::WithParamInterface<int> {};
TEST_P(RecoveryAdmission, ConfirmedRestoreSurvivesAutosaveButRevalidatesDraftAndContext)
{
    const auto candidate = seed(); import(controller, source("current.xyz")); controller.setCell(2); load();
    model.showRecovery(); idle();
    ControlledWork barrier;
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [&](RecoveryPhase phase) {
        if (phase == RecoveryPhase::BeforeCommit) { barrier.entered.set(); barrier.complete.wait(); }
    };
    RecoveryTestAccess::configure(controller, hooks);
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied); barrier.entered.wait();
    select(candidate); ASSERT_EQ(model.workflowPrompt(), "replaceRecovery"); answer("confirm");
    ASSERT_EQ(model.workflowPrompt(), "dirty"); const auto token = model.workflowToken(); answer("discard");
    EXPECT_TRUE(model.workflowPending()); EXPECT_EQ(model.cell(), 2);
    if (GetParam() == 1) model.editCell("3");
    if (GetParam() == 2) RecoveryTestAccess::epoch(controller); // ABA guard independent of the input stamp
    // Repeated and nested old answers cannot grant another action.
    QObject observer; int notifications = 0;
    QObject::connect(&model, &ProjectViewModel::workflowChanged, &observer, [&] {
        ++notifications; model.answerWorkflow(token, "discard");
        model.chooseRecovery(token, candidate.sessionId.toString(QUuid::WithoutBraces), "reject");
    });
    barrier.complete.set(); idle();
    if (GetParam() == 1) {
        ASSERT_EQ(model.workflowPrompt(), "draft"); answer("discard"); idle();
    }
    if (GetParam() == 2) { EXPECT_DOUBLE_EQ(model.cell(), 2); EXPECT_FALSE(model.workflowPending()); }
    else { EXPECT_DOUBLE_EQ(model.cell(), .125); EXPECT_FALSE(model.canUndo()); }
    const auto operation = controller.operation().id;
    for (int i = 0; i != 12; ++i) QCoreApplication::processEvents();
    EXPECT_EQ(controller.operation().id, operation); EXPECT_LT(notifications, 60);
}
INSTANTIATE_TEST_SUITE_P(DeferredAdmission, RecoveryAdmission, testing::Range(0, 3));

class RecoveryClosing : public RecoveryGui, public testing::WithParamInterface<int> {};
TEST_P(RecoveryClosing, ProjectCloseWindowCloseAndQuitDrainRecoveryWorkerWithoutImplicitReject)
{
    const auto candidate = seed(); const auto original = bytes(recordPath(candidate)); load();
    ControlledWork barrier;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false,
        [&] { barrier.entered.set(); barrier.complete.wait(); });
    const bool reject = GetParam() / 3 == 1, discovery = GetParam() / 3 == 2;
    if (discovery) answer("refresh");
    else { select(candidate, reject ? "reject" : "restore"); if (reject) answer("confirm"); }
    barrier.entered.wait(); EXPECT_TRUE(model.operationActive()); EXPECT_FALSE(model.canEditCell());
    const auto route = GetParam() % 3;
    std::unique_ptr<ApplicationShutdown> shutdown;
    const bool previousQuit = QGuiApplication::quitOnLastWindowClosed();
    if (route == 0) model.closeProject();
    else if (route == 1) EXPECT_FALSE(window->close());
    else {
        shutdown = std::make_unique<ApplicationShutdown>(*qobject_cast<QGuiApplication *>(QCoreApplication::instance()), controller, *window);
        QCoreApplication::postEvent(QCoreApplication::instance(), new QEvent(QEvent::Quit));
        ASSERT_TRUE(spinUntil([&] { return controller.shutdownState() == ShutdownState::Closing; }));
    }
    barrier.complete.set(); idle();
    if (route != 0) {
        ASSERT_TRUE(spinUntil([&] { return controller.shutdownState() == ShutdownState::Ready; }));
        EXPECT_FALSE(window->isVisible());
    } else EXPECT_TRUE(window->isVisible());
    EXPECT_FALSE(model.hasProject());
    auto found = discoverRecovery(RecoveryTestAccess::root(controller)); ASSERT_TRUE(found); ASSERT_EQ(found.value().size(), 1);
    EXPECT_NE(found.value()[0].session, RecoverySessionState::Rejected);
    EXPECT_EQ(found.value()[0].record->state->snapshot.cell, candidate.record->state->snapshot.cell);
    EXPECT_TRUE(QFile::exists(QString::fromStdWString(candidate.record->state->content.cachePath().wstring())));
    if (reject || discovery) EXPECT_EQ(bytes(recordPath(candidate)), original);
    QGuiApplication::setQuitOnLastWindowClosed(previousQuit);
}
INSTANTIATE_TEST_SUITE_P(LifecycleRoutes, RecoveryClosing, testing::Range(0, 9));

class FailedRecoveryReplacement : public RecoveryGui, public testing::WithParamInterface<bool> {};
TEST_P(FailedRecoveryReplacement, FailureOrCancellationRetainsTheDirtyCurrentSessionAndHistory)
{
    const auto candidate = seed(); import(controller, source("current.xyz")); controller.setCell(2); load();
    const auto identity = model.runtimeIdentity(); const auto original = bytes(recordPath(candidate));
    model.showRecovery(); idle(); select(candidate); answer("confirm"); ASSERT_EQ(model.workflowPrompt(), "dirty");
    ControlledWork barrier;
    if (GetParam()) OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false,
        [&] { barrier.entered.set(); barrier.complete.wait(); });
    else write(recordPath(candidate), "externally changed");
    answer("discard");
    if (GetParam()) { barrier.entered.wait(); model.cancelOperation(); barrier.complete.set(); }
    idle(); EXPECT_EQ(model.runtimeIdentity(), identity); EXPECT_DOUBLE_EQ(model.cell(), 2);
    EXPECT_TRUE(model.dirty()); EXPECT_TRUE(model.canUndo()); EXPECT_TRUE(model.cacheAvailable());
    EXPECT_EQ(model.sourceName(), "current.xyz"); EXPECT_TRUE(model.projectPath().isEmpty());
    EXPECT_EQ(model.workflowPrompt(), "recovery");
    EXPECT_EQ(controller.operation().status, GetParam() ? OperationStatus::Cancelled : OperationStatus::Failed);
    if (!GetParam()) { EXPECT_EQ(bytes(recordPath(candidate)), "externally changed"); write(recordPath(candidate), original); }
}
INSTANTIATE_TEST_SUITE_P(FailureAndCancellation, FailedRecoveryReplacement, testing::Bool());

class RecoveryRestart : public RecoveryGui, public testing::WithParamInterface<int> {};
TEST_P(RecoveryRestart, NewGuiLaunchPreservesRestoreRejectAndDeferredDecisions)
{
    const auto candidate = seed(); load();
    if (GetParam() == 0) { select(candidate); idle(); ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied); idle(); }
    else if (GetParam() == 1) { select(candidate, "reject"); answer("confirm"); idle(); answer("cancel"); }
    else answer("cancel");
    idle();
    ProjectController next(storage); ProjectViewModel nextModel(next); QQmlApplicationEngine nextEngine;
    QQmlEngine::setObjectOwnership(&nextModel, QQmlEngine::CppOwnership);
    nextEngine.setInitialProperties({{"project", QVariant::fromValue(&nextModel)}, {"fileDialogEnabled", false}});
    nextEngine.loadFromModule("CADContour2D", "Main"); ASSERT_EQ(nextEngine.rootObjects().size(), 1);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(next); return nextModel.workflowPrompt() == "recovery"; }));
    ASSERT_EQ(nextModel.recoveryCandidates().size(), 1);
    const auto metadata = nextModel.recoveryCandidates()[0].toMap();
    EXPECT_EQ(metadata["eligible"].toBool(), GetParam() != 1);
    EXPECT_TRUE(metadata["description"].toString().contains(GetParam() == 1 ? "Rejected" : "Closed"));
    EXPECT_FALSE(nextModel.selectingFile()); EXPECT_FALSE(nextModel.hasProject());
    nextModel.answerWorkflow(nextModel.workflowToken(), "cancel"); settle(next);
}
INSTANTIATE_TEST_SUITE_P(RestartOutcomes, RecoveryRestart, testing::Range(0, 3));

TEST_F(RecoveryGui, ImportSaveCloseOpenAndSaveAsRoundTripWithoutSource)
{
    load(); const auto cloud = source();
    model.selectImportFile(model.selectionToken(), QUrl::fromLocalFile(cloud)); idle();
    model.editCell("0.25"); model.save(); ASSERT_EQ(model.workflowPrompt(), "draft"); answer("apply");
    ASSERT_EQ(model.workflowPrompt(), "save"); const auto first = area.filePath("first/project.pc2dproj"); choose(first);
    ASSERT_FALSE(model.dirty()); model.saveAs(); const auto second = area.filePath("second/project.pc2dproj"); choose(second);
    ASSERT_EQ(model.projectPath(), second); EXPECT_TRUE(QFile::exists(first));
    const auto owned = controller.project()->dataset()->cachePath(); model.closeProject(); idle();
    ASSERT_FALSE(model.hasProject()); ASSERT_TRUE(QFile::remove(cloud));
    model.open(); choose(second); ASSERT_TRUE(model.hasProject());
    EXPECT_DOUBLE_EQ(model.cell(), .25); EXPECT_FALSE(model.dirty()); EXPECT_FALSE(model.canUndo()); EXPECT_FALSE(model.canRedo());
    EXPECT_EQ(controller.project()->dataset()->cachePath(), owned);
    EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0); EXPECT_EQ(controller.pointCacheAvailability()->reads.readBytes, 73728);
    EXPECT_EQ(model.densityMapStatusText(), QStringLiteral("Карта плотности отсутствует"));
    SaveTestAccess::configure(controller, [](SavePhase phase) {
        EXPECT_NE(phase, SavePhase::CopyRead); EXPECT_NE(phase, SavePhase::CopyWrite); EXPECT_NE(phase, SavePhase::CopyVerify);
    });
    controller.setCell(.5); model.save(); idle(); EXPECT_FALSE(model.dirty());
    EXPECT_EQ(controller.project()->dataset()->cachePath(), owned);
}

} // namespace
