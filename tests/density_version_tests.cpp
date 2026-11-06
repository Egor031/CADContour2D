#include "operation_test_support.h"
#include "project_test_access.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <bit>
#include <limits>
#include <map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace cadcontour {
class DensityVersionTestAccess {
public:
    static void stopAutosave(ProjectController &controller)
    { controller.autosaveSoon_.stop(); controller.autosaveTimer_.stop(); }
    static void prepare(ProjectController &controller, std::function<void()> hook)
    { controller.prepareDensityHook_ = std::move(hook); }
    static void history(ProjectController &controller, std::function<void()> hook)
    { controller.prepareHistoryHook_ = std::move(hook); }
    static void identity(ProjectController &controller, std::function<VersionId()> factory)
    { controller.versionIdFactory_ = std::move(factory); }
    static void precise(ProjectController &controller, bool required)
    { controller.session_->preciseResetRequired = required; ++controller.session_->preciseGeneration; }
    static bool preciseRequired(const ProjectController &controller)
    { return controller.session_->preciseResetRequired; }
    static void historyGeneration(ProjectController &controller, std::uint64_t value)
    { controller.session_->history.generation_ = value; }
    static std::uint64_t historyGeneration(const ProjectController &controller)
    { return controller.session_->history.generation(); }
    static void choiceGeneration(ProjectController &controller, std::uint64_t value)
    { controller.session_->densityChoiceGeneration = value; }
    static std::uint64_t choiceGeneration(const ProjectController &controller)
    { return controller.session_->densityChoiceGeneration; }
    static void preciseGeneration(ProjectController &controller, std::uint64_t value)
    { controller.session_->preciseGeneration = value; }
    static std::uint64_t preciseGeneration(const ProjectController &controller)
    { return controller.session_->preciseGeneration; }
};
} // namespace cadcontour

namespace {
using namespace cadcontour;
using namespace cadcontour::test;
using U64 = std::uint64_t;
using Counts = std::map<std::pair<U64, U64>, U64>;

QString path(const DensityVersion &version)
{ return QString::fromStdWString(version.descriptor().dataset.location->path.wstring()); }

QByteArray digest(const QString &name)
{
    QFile file(name);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("fixture open");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) throw std::runtime_error("fixture hash");
    return hash.result();
}

class DensityVersions : public testing::Test {
protected:
    QTemporaryDir area;
    QString root = area.path() + QStringLiteral("/versions");
    QString source = area.path() + QStringLiteral("/source.xyz");
    ProjectController controller{root};

    void initial(const QByteArray &text = "0 0 0\n0 0 0\n2 2 0\n")
    {
        ASSERT_TRUE(area.isValid());
        QFile file(source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(text), text.size());
        file.close();
        ASSERT_EQ(controller.importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}}).outcome,
                  CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        DensityVersionTestAccess::stopAutosave(controller);
    }
    void build(DensityBuildRequest request = {})
    {
        ASSERT_EQ(controller.buildDensity(request).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        ASSERT_EQ(controller.operation().decision, PublicationDecision::Accept);
    }
    void ready()
    {
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_TRUE(context);
        ASSERT_TRUE(waitContext(context, false));
    }
    QString pendingPath()
    {
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        std::lock_guard lock(context->mutex);
        return context->outcome.density->completedPath;
    }
    VersionId active() const { return controller.project()->activeDensityVersion().value(); }
    const DensityVersion &version(VersionId id) const { return *controller.project()->densityVersion(id); }
    void verify(const DensityVersion &version, U64 width, U64 height, const Counts &expected)
    {
        auto opened = density::Reader::openReader(path(version), density::initialResourcePolicy());
        ASSERT_TRUE(opened);
        auto &reader = *opened.value();
        EXPECT_EQ(reader.metadata().id, version.descriptor().dataset.id);
        EXPECT_EQ(reader.metadata().source, version.descriptor().source);
        EXPECT_EQ(reader.metadata().sourceMeasurements, version.descriptor().sourceMeasurements);
        EXPECT_EQ(reader.mapping().width(), width);
        EXPECT_EQ(reader.mapping().height(), height);
        EXPECT_EQ(std::bit_cast<U64>(reader.mapping().cell()),
                  std::bit_cast<U64>(version.descriptor().mapping.cell()));
        const auto side = reader.metadata().tileSide;
        for (U64 ty = 0; ty <= (height - 1) / side; ++ty)
            for (U64 tx = 0; tx <= (width - 1) / side; ++tx) {
                auto tile = reader.readTile({tx, ty});
                ASSERT_TRUE(tile);
                const auto &value = tile.value();
                for (U64 y = 0; y < value.shape.height; ++y)
                    for (U64 x = 0; x < value.shape.width; ++x) {
                        auto found = expected.find({tx * side + x, ty * side + y});
                        EXPECT_EQ(value.counts[static_cast<std::size_t>(y * value.shape.width + x)],
                                  found == expected.end() ? 0 : found->second);
                    }
            }
        ASSERT_TRUE(reader.fullValidate());
    }
};

TEST_F(DensityVersions, ProductionBuildUsesLiteralBoundaryCountsAndRetainsImmutableFiles)
{
    initial("0 0 0\n0 0 0\n255 255 0\n256 256 0\n256 256 0\n511 511 0\n"
            "512 512 0\n512 512 0\n512 512 0\n1025 771 0\n1025 771 0\n");
    ASSERT_TRUE(controller.project()->densityVersions().empty());
    EXPECT_FALSE(controller.project()->activeDensityVersion());
    const auto cache = QString::fromStdWString(controller.project()->dataset()->cachePath().wstring());
    const auto cacheHash = digest(cache);
    ASSERT_TRUE(QFile::remove(source));
    DensityBuildRequest request; request.eventCapacity = 3;
    build(request);
    const auto a = active();
    const auto *aAddress = &version(a);
    const auto &first = version(a);
    const auto oldHash = digest(path(first));
    verify(first, 1025, 771, {{{0, 0}, 2}, {{255, 255}, 1}, {{256, 256}, 2},
                            {{511, 511}, 1}, {{512, 512}, 3}, {{1024, 770}, 2}});
    build(request);
    const auto b = active();
    EXPECT_NE(a, b);
    EXPECT_NE(first.descriptor().dataset.id, version(b).descriptor().dataset.id);
    EXPECT_NE(&first.rough(), &version(b).rough());
    EXPECT_EQ(&version(a), aAddress);
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
    EXPECT_EQ(digest(path(first)), oldHash);
    EXPECT_EQ(digest(cache), cacheHash);
    const auto &metadata = first.descriptor().build;
    EXPECT_EQ(metadata.method, DensityBuildMethod::C1);
    EXPECT_EQ(metadata.contract, DensityCountContract::CompletePointCache);
    EXPECT_EQ(metadata.tileSide, 256);
    EXPECT_EQ(metadata.groupSide, 512);
    EXPECT_EQ(metadata.eventCapacity, 3);
    EXPECT_EQ(metadata.statistics.scanned, 11);
    EXPECT_EQ(metadata.statistics.scanAllCalls, 1);
    EXPECT_EQ(metadata.statistics.spatialQueries, 0);
    EXPECT_EQ(metadata.statistics.eventsWritten, 11);
    EXPECT_EQ(metadata.statistics.eventsRead, 11);
    EXPECT_EQ(metadata.statistics.tileTotal, 11);
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    build(request);
    EXPECT_EQ(version(active()).descriptor().mapping.width(), 2050);
    EXPECT_EQ(version(active()).descriptor().mapping.height(), 1542);
    EXPECT_EQ(first.descriptor().mapping.cell(), 1.0);
    EXPECT_EQ(digest(path(first)), oldHash);
}

TEST_F(DensityVersions, FirstBuildUndoToNullKeepsCatalogueDirtyAndRedoSelectsSameId)
{
    initial(); build();
    const auto id = active();
    const auto file = path(version(id));
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_FALSE(controller.project()->activeDensityVersion());
    EXPECT_EQ(controller.project()->densityVersions().size(), 1);
    EXPECT_TRUE(controller.isDirty());
    EXPECT_TRUE(QFileInfo::exists(file));
    ASSERT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(active(), id);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
}

TEST_F(DensityVersions, ImportReplacementRetainsAcceptedDatasetAndDropsOnlySessionHistory)
{
    initial(); build();
    const auto retained = controller.project()->densityVersions().front();
    const auto file = path(*retained); const auto hash = digest(file);
    const auto previousRuntime = controller.project()->runtimeIdentity();
    ASSERT_EQ(controller.importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}, true}).outcome,
              CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    DensityVersionTestAccess::stopAutosave(controller);
    EXPECT_NE(controller.project()->runtimeIdentity(), previousRuntime);
    EXPECT_TRUE(controller.project()->densityVersions().empty());
    EXPECT_FALSE(controller.project()->activeDensityVersion());
    EXPECT_FALSE(controller.canUndo());
    EXPECT_EQ(digest(file), hash);
    verify(*retained, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
}

TEST_F(DensityVersions, CatalogueKeepsNoOpenHandleAndFilesOutliveController)
{
    initial(); build(); const auto first = controller.project()->densityVersions().front();
    build();
    for (const auto &entry : controller.project()->densityVersions()) {
        const auto file = path(*entry);
        const auto handle = CreateFileW(reinterpret_cast<const wchar_t *>(file.utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        CloseHandle(handle);
    }
    QString retainedPath;
    {
        ProjectController other{area.path() + QStringLiteral("/other")};
        ASSERT_EQ(other.importAscXyz({source, {8ULL << 20, 64ULL << 20, 8}}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(other));
        DensityVersionTestAccess::stopAutosave(other);
        ASSERT_EQ(other.buildDensity().outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(other));
        ASSERT_EQ(other.operation().decision, PublicationDecision::Accept);
        retainedPath = path(*other.project()->densityVersions().front());
    }
    EXPECT_TRUE(QFileInfo::exists(retainedPath));
    EXPECT_TRUE(QFileInfo::exists(path(*first)));
    RecordProperty("descriptor_bytes", std::to_string(sizeof(DensityVersionDescriptor)));
    RecordProperty("version_bytes", std::to_string(sizeof(DensityVersion)));
}

TEST_F(DensityVersions, SelectionHistoryNoOpAndRedoTruncationNeverRemoveVersions)
{
    initial(); build(); const auto a = active();
    build(); const auto b = active();
    ASSERT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(active(), b);
    const auto index = ProjectControllerTestAccess::index(controller);
    EXPECT_EQ(controller.selectDensityVersion(b).outcome, CommandOutcome::NoChange);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    ASSERT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(active(), a);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.setCell(0.25).outcome, CommandOutcome::Applied);
    EXPECT_FALSE(controller.canRedo());
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
    EXPECT_TRUE(QFileInfo::exists(path(version(a))));
    EXPECT_TRUE(QFileInfo::exists(path(version(b))));
    EXPECT_EQ(controller.selectDensityVersion({}).outcome, CommandOutcome::Rejected);
    VersionId foreign; foreign.bytes.fill(std::byte{0x43});
    EXPECT_EQ(controller.selectDensityVersion(foreign).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(active(), b);
}

TEST_F(DensityVersions, MixedCellBuildAndSelectionHistoryPreservesChronology)
{
    initial();
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    build(); const auto a = active();
    ASSERT_EQ(controller.setCell(0.25).outcome, CommandOutcome::Applied);
    build(); const auto b = active();
    ASSERT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), b);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), a);
    EXPECT_EQ(controller.project()->cell(), 0.25);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(controller.project()->cell(), 0.5);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_FALSE(controller.project()->activeDensityVersion());
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(controller.project()->cell(), 1.0);
    for (int i = 0; i != 5; ++i) ASSERT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(active(), a);
    EXPECT_EQ(controller.project()->cell(), 0.25);
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
}

TEST_F(DensityVersions, SelectionAndFutureCellUndoRedoAtReadyDoNotRevokeBuild)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ready();
    ASSERT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), b);
    ASSERT_EQ(controller.redo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), a);
    ASSERT_EQ(controller.setCell(0.125).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
    const auto c = active();
    EXPECT_NE(c, a); EXPECT_NE(c, b);
    EXPECT_EQ(version(c).descriptor().mapping.cell(), 1.0);
    EXPECT_EQ(controller.project()->cell(), 0.125);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), a);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(controller.project()->cell(), 1.0);
}

TEST_F(DensityVersions, ReentrantPreparationRetainsLateSelectionAndCellHistory)
{
    initial(); build(); const auto a = active(); build();
    DensityVersionTestAccess::prepare(controller, [&] {
        EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.setCell(0.75).outcome, CommandOutcome::Applied);
    });
    build();
    EXPECT_EQ(controller.project()->cell(), 0.75);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), a);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(controller.project()->cell(), 1.0);
}

TEST_F(DensityVersions, IdentityAndHistoryPreparationFailuresAreAtomic)
{
    initial(); build(); const auto a = active();
    const auto file = path(version(a)); const auto hash = digest(file);
    const auto count = ProjectControllerTestAccess::count(controller);
    for (int failure = 0; failure != 3; ++failure) {
        if (failure == 0) DensityVersionTestAccess::identity(controller, [] { return VersionId{}; });
        if (failure == 1) DensityVersionTestAccess::identity(controller, [a] { return a; });
        if (failure == 2) {
            DensityVersionTestAccess::identity(controller, {});
            DensityVersionTestAccess::history(controller, [] { throw std::bad_alloc(); });
        }
        ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        EXPECT_NE(controller.operation().decision, PublicationDecision::Accept);
        EXPECT_EQ(active(), a);
        EXPECT_EQ(controller.project()->densityVersions().size(), 1);
        EXPECT_EQ(ProjectControllerTestAccess::count(controller), count);
        EXPECT_EQ(digest(file), hash);
    }
}

TEST_F(DensityVersions, CancelAndRelevantInvalidationLeaveCatalogueAndHistoryUntouched)
{
    initial(); build(); const auto a = active();
    const auto count = ProjectControllerTestAccess::count(controller);
    for (bool invalidate : {false, true}) {
        ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
        ready();
        if (invalidate) OperationTestAccess::invalidate(controller);
        else EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_EQ(active(), a);
        EXPECT_EQ(controller.project()->densityVersions().size(), 1);
        EXPECT_EQ(ProjectControllerTestAccess::count(controller), count);
    }
}

TEST_F(DensityVersions, AcceptedFilesSurviveCloseBeforeWorkerOwnershipHandoff)
{
    initial();
    Event handoff, allowHandoff;
    OperationRunnerTestAccess::beforeOwnershipFinalization(OperationTestAccess::runner(controller),
        [&](PublicationDecision decision) {
            EXPECT_EQ(decision, PublicationDecision::Accept); handoff.set(); allowHandoff.wait();
        });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return handoff.isSet(); }));
    const auto owner = controller.project()->densityVersions().back();
    const auto file = path(*owner); const auto hash = digest(file);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.closeProject(true).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(digest(file), hash);
    allowHandoff.set();
    ASSERT_TRUE(finish(controller));
    verify(*owner, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
    EXPECT_EQ(digest(file), hash);
}

TEST_F(DensityVersions, ProductionCommitAndHandoffDoNotReadExclusivelyLockedDataset)
{
    initial();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ready();
    const auto file = pendingPath();
    const auto handle = CreateFileW(reinterpret_cast<const wchar_t *>(file.utf16()),
        GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const bool finished = finish(controller);
    CloseHandle(handle);
    ASSERT_TRUE(finished);
    ASSERT_EQ(controller.operation().decision, PublicationDecision::Accept);
    ASSERT_EQ(controller.project()->densityVersions().size(), 1);
    verify(version(active()), 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
    const auto id = controller.operation().id;
    OperationTestAccess::ready(controller, id);
    EXPECT_EQ(controller.project()->densityVersions().size(), 1);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
}

class DensityVersionReentry : public DensityVersions, public testing::WithParamInterface<int> {};
TEST_P(DensityVersionReentry, ReentrantCloseShutdownOrDetachDiscardsWithoutPublishing)
{
    initial(); build(); const auto a = active();
    const auto previousFile = path(version(a));
    const auto previousHash = digest(previousFile);
    DensityVersionTestAccess::prepare(controller, [&] {
        if (GetParam() == 0) EXPECT_EQ(controller.closeProject(true).outcome, CommandOutcome::Applied);
        if (GetParam() == 1) EXPECT_EQ(controller.requestShutdown(true).outcome, CommandOutcome::Applied);
        if (GetParam() == 2) EXPECT_EQ(controller.setDensityReceiver({}).outcome, CommandOutcome::Applied);
    });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ready(); const auto pending = pendingPath();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_FALSE(QFileInfo::exists(pending));
    EXPECT_EQ(digest(previousFile), previousHash);
    if (GetParam() == 0) EXPECT_EQ(controller.project(), nullptr);
    if (GetParam() == 1) EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    if (GetParam() == 2) {
        ASSERT_TRUE(controller.project());
        EXPECT_EQ(active(), a);
        EXPECT_EQ(controller.project()->densityVersions().size(), 1);
        EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
    }
}
INSTANTIATE_TEST_SUITE_P(Lifecycle, DensityVersionReentry, testing::Values(0, 1, 2));

TEST_F(DensityVersions, NotificationsObserveCatalogueSelectionAndHistoryTogether)
{
    initial();
    unsigned observed = 0;
    const auto connection = QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        if (!controller.project() || controller.project()->densityVersions().empty()) return;
        ++observed;
        EXPECT_TRUE(controller.project()->activeDensityVersion());
        EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
        EXPECT_EQ(ProjectControllerTestAccess::index(controller), 1);
        EXPECT_TRUE(controller.canUndo());
        EXPECT_TRUE(controller.isDirty());
    });
    build();
    QObject::disconnect(connection);
    EXPECT_EQ(observed, 1);
}

TEST_F(DensityVersions, PreciseGuardCancelConfirmAndNoOpRunBeforeHistoryMutation)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    DensityVersionTestAccess::precise(controller, true);
    unsigned confirmations = 0;
    ASSERT_EQ(controller.setPreciseResetHandler([&](const PreciseResetRequest &request) {
        ++confirmations;
        EXPECT_EQ(request.action, DensityTransitionKind::Select);
        EXPECT_EQ(request.before, b); EXPECT_EQ(request.target, a);
        EXPECT_EQ(request.projectId, controller.project()->projectId());
        EXPECT_EQ(request.runtimeIdentity, controller.project()->runtimeIdentity());
        return PreciseResetDecision::Cancel;
    }).outcome, CommandOutcome::Applied);
    const auto index = ProjectControllerTestAccess::index(controller);
    EXPECT_EQ(controller.selectDensityVersion(b).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(confirmations, 0);
    EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(confirmations, 1);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
    ASSERT_EQ(controller.setPreciseResetHandler([](const auto &) { return PreciseResetDecision::Confirm; }).outcome,
              CommandOutcome::Applied);
    EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Applied);
    EXPECT_FALSE(DensityVersionTestAccess::preciseRequired(controller));
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(active(), b);
    EXPECT_FALSE(DensityVersionTestAccess::preciseRequired(controller));
}

TEST_F(DensityVersions, PreciseGuardUndoRedoRefusalKeepsHistoryCursor)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    for (auto decision : {PreciseResetDecision::NeedsConfirmation, PreciseResetDecision::Cancel,
                          PreciseResetDecision::Stale}) {
        DensityVersionTestAccess::precise(controller, true);
        ASSERT_EQ(controller.setPreciseResetHandler([decision](const auto &) { return decision; }).outcome,
                  CommandOutcome::Applied);
        const auto index = ProjectControllerTestAccess::index(controller);
        EXPECT_EQ(controller.undo().outcome, CommandOutcome::Rejected);
        EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
        EXPECT_EQ(active(), b);
    }
    ASSERT_EQ(controller.setPreciseResetHandler([](const auto &) { return PreciseResetDecision::Confirm; }).outcome,
              CommandOutcome::Applied);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), a);
    DensityVersionTestAccess::precise(controller, true);
    ASSERT_EQ(controller.setPreciseResetHandler([](const auto &request) {
        EXPECT_EQ(request.action, DensityTransitionKind::Redo);
        return PreciseResetDecision::Cancel;
    }).outcome, CommandOutcome::Applied);
    const auto index = ProjectControllerTestAccess::index(controller);
    EXPECT_EQ(controller.redo().outcome, CommandOutcome::Rejected);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    EXPECT_EQ(active(), a);
}

TEST_F(DensityVersions, PreciseGuardRejectsStaleReentrantAnswerAndDoesNotResetOnPreparationFailure)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    DensityVersionTestAccess::precise(controller, true);
    ASSERT_EQ(controller.setPreciseResetHandler([&](const auto &) {
        DensityVersionTestAccess::precise(controller, true);
        return PreciseResetDecision::Confirm;
    }).outcome, CommandOutcome::Applied);
    const auto index = ProjectControllerTestAccess::index(controller);
    EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
    ASSERT_EQ(controller.setPreciseResetHandler([](const auto &) { return PreciseResetDecision::Confirm; }).outcome,
              CommandOutcome::Applied);
    DensityVersionTestAccess::history(controller, [] { throw std::bad_alloc(); });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_NE(controller.operation().decision, PublicationDecision::Accept);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
}

TEST_F(DensityVersions, PreciseBuildConfirmationPreservesLateFutureCellUndoRedo)
{
    initial(); build(); const auto a = active();
    DensityVersionTestAccess::precise(controller, true);
    ASSERT_EQ(controller.setPreciseResetHandler([&](const PreciseResetRequest &request) {
        EXPECT_EQ(request.action, DensityTransitionKind::Build);
        EXPECT_EQ(controller.setCell(0.125).outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.redo().outcome, CommandOutcome::Applied);
        return PreciseResetDecision::Confirm;
    }).outcome, CommandOutcome::Applied);
    build();
    EXPECT_NE(active(), a);
    EXPECT_EQ(version(active()).descriptor().mapping.cell(), 1.0);
    EXPECT_EQ(controller.project()->cell(), 0.125);
    EXPECT_FALSE(DensityVersionTestAccess::preciseRequired(controller));
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), a);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(controller.project()->cell(), 1.0);
}

TEST_F(DensityVersions, ThrowingPrecisePolicyRejectsSelectionAndUndoWithoutCursorChanges)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    DensityVersionTestAccess::precise(controller, true);
    ASSERT_EQ(controller.setPreciseResetHandler([](const PreciseResetRequest &) -> PreciseResetDecision {
        throw std::runtime_error("injected reset policy failure");
    }).outcome, CommandOutcome::Applied);
    const auto index = ProjectControllerTestAccess::index(controller);
    auto selected = controller.selectDensityVersion(a);
    EXPECT_EQ(selected.outcome, CommandOutcome::Rejected);
    ASSERT_TRUE(selected.failure);
    EXPECT_EQ(selected.failure->detail, ErrorDetail::PreciseResetPolicyFailed);
    auto undone = controller.undo();
    EXPECT_EQ(undone.outcome, CommandOutcome::Rejected);
    ASSERT_TRUE(undone.failure);
    EXPECT_EQ(undone.failure->detail, ErrorDetail::PreciseResetPolicyFailed);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
}

TEST_F(DensityVersions, RevisionExhaustionRejectsSelectionAndBuildWithoutWraparound)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    ProjectControllerTestAccess::setRevision(controller, std::numeric_limits<U64>::max());
    const auto index = ProjectControllerTestAccess::index(controller);
    EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(active(), b);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_NE(controller.operation().decision, PublicationDecision::Accept);
    EXPECT_EQ(controller.project()->inputRevision(), std::numeric_limits<U64>::max());
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
}

TEST_F(DensityVersions, HistoryGenerationExhaustionRejectsBeforeSelectionOrCursorMutation)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    DensityVersionTestAccess::historyGeneration(controller, std::numeric_limits<U64>::max());
    const auto index = ProjectControllerTestAccess::index(controller);
    const auto revision = controller.project()->inputRevision();
    EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Rejected);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(controller.project()->inputRevision(), revision);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_NE(controller.operation().decision, PublicationDecision::Accept);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
}

TEST_F(DensityVersions, BuildConfirmationSelectionABADiscardsOuterActionWithoutSecondReset)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    ASSERT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Applied);
    DensityVersionTestAccess::precise(controller, true);
    const auto beforeChoice = DensityVersionTestAccess::choiceGeneration(controller);
    const auto beforePrecise = DensityVersionTestAccess::preciseGeneration(controller);
    const auto beforeCount = ProjectControllerTestAccess::count(controller);
    unsigned outerAnswers = 0;
    ASSERT_EQ(controller.setPreciseResetHandler([&](const PreciseResetRequest &request) {
        ++outerAnswers;
        EXPECT_EQ(request.action, DensityTransitionKind::Build);
        EXPECT_EQ(controller.setPreciseResetHandler([](const auto &) {
            return PreciseResetDecision::Confirm;
        }).outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.selectDensityVersion(b).outcome, CommandOutcome::Applied);
        EXPECT_FALSE(DensityVersionTestAccess::preciseRequired(controller));
        EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Applied);
        return PreciseResetDecision::Confirm;
    }).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(outerAnswers, 1);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(active(), a);
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), beforeCount + 2);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), beforeCount + 2);
    EXPECT_EQ(DensityVersionTestAccess::choiceGeneration(controller), beforeChoice + 2);
    EXPECT_EQ(DensityVersionTestAccess::preciseGeneration(controller), beforePrecise + 1);
    EXPECT_FALSE(DensityVersionTestAccess::preciseRequired(controller));
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_EQ(active(), b);
    EXPECT_FALSE(DensityVersionTestAccess::preciseRequired(controller));
}

TEST_F(DensityVersions, UndoConfirmationHistoryEditKeepsLateCommandAndRejectsOldCursorMove)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    DensityVersionTestAccess::precise(controller, true);
    const auto beforeIndex = ProjectControllerTestAccess::index(controller);
    ASSERT_EQ(controller.setPreciseResetHandler([&](const PreciseResetRequest &request) {
        EXPECT_EQ(request.action, DensityTransitionKind::Undo);
        EXPECT_EQ(request.before, b); EXPECT_EQ(request.target, a);
        EXPECT_EQ(controller.setCell(0.25).outcome, CommandOutcome::Applied);
        return PreciseResetDecision::Confirm;
    }).outcome, CommandOutcome::Applied);
    const auto result = controller.undo();
    EXPECT_EQ(result.outcome, CommandOutcome::Rejected);
    ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.failure->detail, ErrorDetail::StaleConfirmation);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(controller.project()->cell(), 0.25);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), beforeIndex + 1);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), beforeIndex + 1);
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project()->cell(), 1.0);
    EXPECT_EQ(active(), b);
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
}

TEST_F(DensityVersions, BuildRebaseExhaustionAfterConfirmationPreservesPreciseAndLateCell)
{
    initial(); build(); const auto a = active();
    const auto oldFile = path(version(a)); const auto oldHash = digest(oldFile);
    DensityVersionTestAccess::precise(controller, true);
    const auto preciseGeneration = DensityVersionTestAccess::preciseGeneration(controller);
    ASSERT_EQ(controller.setPreciseResetHandler([&](const PreciseResetRequest &request) {
        EXPECT_EQ(request.action, DensityTransitionKind::Build);
        EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
        DensityVersionTestAccess::historyGeneration(controller, std::numeric_limits<U64>::max());
        return PreciseResetDecision::Confirm;
    }).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(active(), a);
    EXPECT_EQ(controller.project()->densityVersions().size(), 1);
    EXPECT_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 2);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), 2);
    EXPECT_EQ(DensityVersionTestAccess::historyGeneration(controller), std::numeric_limits<U64>::max());
    EXPECT_EQ(DensityVersionTestAccess::preciseGeneration(controller), preciseGeneration);
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
    EXPECT_EQ(digest(oldFile), oldHash);
}

TEST_F(DensityVersions, ChoiceGenerationExhaustionRefusesTransitionsBeforeMutation)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    DensityVersionTestAccess::choiceGeneration(controller, std::numeric_limits<U64>::max());
    const auto revision = controller.project()->inputRevision();
    const auto index = ProjectControllerTestAccess::index(controller);
    EXPECT_EQ(controller.selectDensityVersion(b).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Rejected);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
    EXPECT_EQ(controller.project()->inputRevision(), revision);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    EXPECT_EQ(DensityVersionTestAccess::choiceGeneration(controller), std::numeric_limits<U64>::max());
}

TEST_F(DensityVersions, PreciseGenerationExhaustionDoesNotBlockFutureCellOrNoOpChoice)
{
    initial(); build(); const auto a = active(); build(); const auto b = active();
    DensityVersionTestAccess::precise(controller, true);
    DensityVersionTestAccess::preciseGeneration(controller, std::numeric_limits<U64>::max());
    unsigned callbacks = 0;
    ASSERT_EQ(controller.setPreciseResetHandler([&](const auto &) {
        ++callbacks; return PreciseResetDecision::Confirm;
    }).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.selectDensityVersion(b).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.setCell(0.25).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project()->cell(), 1.0);
    const auto revision = controller.project()->inputRevision();
    const auto index = ProjectControllerTestAccess::index(controller);
    EXPECT_EQ(controller.selectDensityVersion(a).outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Rejected);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ(active(), b);
    EXPECT_EQ(controller.project()->densityVersions().size(), 2);
    EXPECT_EQ(controller.project()->inputRevision(), revision);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    EXPECT_EQ(DensityVersionTestAccess::preciseGeneration(controller), std::numeric_limits<U64>::max());
    EXPECT_TRUE(DensityVersionTestAccess::preciseRequired(controller));
}

} // namespace
