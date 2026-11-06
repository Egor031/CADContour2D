#include "operation_test_support.h"
#include "project_test_access.h"
#include "application/density_operation.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace cadcontour {
class DensityOperationTestAccess {
public:
    static void stopAutosave(ProjectController &controller)
    {
        controller.autosaveSoon_.stop();
        controller.autosaveTimer_.stop();
    }
    static void advanceEpoch(ProjectController &controller) { ++controller.contextEpoch_; }
    static void epoch(ProjectController &controller, std::uint64_t value) { controller.contextEpoch_ = value; }
    static CommandResult start(ProjectController &controller,
        std::function<void(density::BuildPhase)> progress = {},
        std::function<void(WorkerOutcome &)> transform = {},
        std::function<void()> beforeReady = {})
    {
        auto snapshot = std::make_shared<DensityBuildSnapshot>();
        snapshot->projectId = controller.project()->projectId();
        snapshot->source = *controller.project()->dataset();
        snapshot->cell = controller.project()->cell();
        snapshot->outputScope = controller.storageRoot_ + QStringLiteral("/density-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto hooks = std::make_shared<DensityOperation::TestHooks>();
        hooks->progress = std::move(progress);
        hooks->beforeReady = std::move(beforeReady);
        auto work = DensityOperation::work(snapshot, std::move(hooks));
        return controller.startWork([work = std::move(work), transform = std::move(transform)]
            (const auto &operation, const auto &control) mutable {
                auto outcome = work(operation, control);
                if (transform) transform(outcome);
                return outcome;
            }, OperationKind::DensityBuild, false, snapshot, controller.densityReceiver_);
    }
};
} // namespace cadcontour

namespace {
using namespace cadcontour;
using namespace cadcontour::test;
using U64 = std::uint64_t;
using Cell = std::pair<U64, U64>;
using Counts = std::map<Cell, U64>;
using Descriptor = std::shared_ptr<const DensityReadyProduct>;

QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("fixture open");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) throw std::runtime_error("fixture hash");
    return hash.result();
}

// A catalogue fixture only: entries and their edits are prepared before the
// decision. Commit exchanges owners and selects the new dataset without I/O,
// allocation or destruction of the previous catalogue under the decision lock.
struct Catalogue {
    struct Entry { Descriptor descriptor; int roughEdit = 0; };
    std::vector<Entry> entries;
    density::DatasetId selected;
    unsigned commits = 0;
    QElapsedTimer *clock = nullptr;
    qint64 preparedAt = 0, committedAt = 0;
};
class Receiver final : public DensityPublicationReceiver {
public:
    class Candidate final : public DensityPublicationCandidate {
    public:
        Candidate(std::shared_ptr<Catalogue> owner, Descriptor descriptor)
            : owner_(std::move(owner)), ready_(std::move(descriptor)), entries_(owner_->entries)
        { entries_.push_back({ready_, 0}); }
        const Descriptor &ready() const noexcept override { return ready_; }
        void commit() noexcept override
        {
            owner_->entries.swap(entries_);
            owner_->selected = ready_->metadata.id;
            ++owner_->commits;
            if (owner_->clock) owner_->committedAt = owner_->clock->nsecsElapsed();
        }
        void notify() noexcept override { ++notifications; }
        unsigned notifications = 0;
    private:
        std::shared_ptr<Catalogue> owner_;
        Descriptor ready_;
        std::vector<Catalogue::Entry> entries_;
    };
    std::shared_ptr<Catalogue> owner = std::make_shared<Catalogue>();
    std::function<void(const Descriptor &)> duringPrepare;
    unsigned prepares = 0;
    bool refuse = false;
    bool invalidCandidate = false;
    Descriptor lastBorrowed;
    std::unique_ptr<DensityPublicationCandidate> prepare(Descriptor descriptor) override
    {
        ++prepares;
        if (owner->clock) owner->preparedAt = owner->clock->nsecsElapsed();
        lastBorrowed = descriptor;
        if (duringPrepare) duringPrepare(descriptor);
        if (refuse) return {};
        if (invalidCandidate) return std::make_unique<Candidate>(owner, lastBorrowed = {});
        return std::make_unique<Candidate>(owner, std::move(descriptor));
    }
};

class DensityOperation : public testing::Test {
protected:
    QTemporaryDir area;
    QString root = area.path() + QStringLiteral("/хранилище");
    QString source = area.path() + QStringLiteral("/облако.xyz");
    ProjectController controller{root};
    std::shared_ptr<Receiver> receiver = std::make_shared<Receiver>();
    cache::ResourceLimits limits{8ULL << 20, 64ULL << 20, 8};

    void SetUp() override { ASSERT_TRUE(area.isValid()); }
    void initial(const QByteArray &text = "0 0 0\n0 0 0\n2 2 0\n")
    {
        QFile file(source);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(text), text.size());
        file.close();
        ASSERT_EQ(controller.importAscXyz({source, limits}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        DensityOperationTestAccess::stopAutosave(controller);
        ASSERT_EQ(controller.setDensityReceiver(receiver).outcome, CommandOutcome::Applied);
    }
    QString cachePath() const
    { return QString::fromStdWString(controller.project()->dataset()->cachePath().wstring()); }
    void awaitReady()
    {
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_TRUE(context);
        ASSERT_TRUE(waitContext(context, false));
    }
    Descriptor pending()
    {
        auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        std::lock_guard lock(context->mutex);
        return context->outcome.density;
    }
    void accepted()
    {
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().kind, OperationKind::DensityBuild);
        EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
        EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Succeeded);
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
        EXPECT_FALSE(controller.operation().active);
        ASSERT_FALSE(receiver->owner->entries.empty());
        EXPECT_EQ(receiver->owner->selected, receiver->owner->entries.back().descriptor->metadata.id);
    }
    void verify(const Descriptor &descriptor, U64 width, U64 height, const Counts &expected)
    {
        ASSERT_TRUE(descriptor);
        auto opened = density::Reader::openReader(descriptor->completedPath, density::initialResourcePolicy());
        ASSERT_TRUE(opened) << (opened ? "" : opened.error().detail.toStdString());
        auto &reader = *opened.value();
        EXPECT_EQ(reader.metadata().id, descriptor->metadata.id);
        EXPECT_EQ(reader.metadata().source, descriptor->metadata.source);
        EXPECT_EQ(reader.metadata().contract, density::BuildContract::CompletePointCache);
        EXPECT_EQ(reader.mapping().width(), width);
        EXPECT_EQ(reader.mapping().height(), height);
        EXPECT_EQ(std::bit_cast<U64>(reader.mapping().cell()), std::bit_cast<U64>(descriptor->snapshot->cell));
        std::set<Cell> physical;
        U64 total = 0;
        const auto side = reader.metadata().tileSide;
        for (const auto &[cell, count] : expected) {
            physical.emplace(cell.first / side, cell.second / side);
            total += count;
        }
        EXPECT_EQ(reader.statistics().physicalTiles, physical.size());
        for (U64 tileY = 0; tileY <= (height - 1) / side; ++tileY)
            for (U64 tileX = 0; tileX <= (width - 1) / side; ++tileX) {
                auto tile = reader.readTile({tileX, tileY});
                ASSERT_TRUE(tile);
                const auto w = std::min<U64>(side, width - tileX * side);
                const auto h = std::min<U64>(side, height - tileY * side);
                ASSERT_EQ(tile.value().shape.width, w);
                ASSERT_EQ(tile.value().shape.height, h);
                ASSERT_EQ(tile.value().counts.size(), w * h);
                for (U64 y = 0; y != h; ++y) for (U64 x = 0; x != w; ++x) {
                    auto found = expected.find({tileX * side + x, tileY * side + y});
                    ASSERT_EQ(tile.value().counts[static_cast<std::size_t>(y * w + x)],
                        found == expected.end() ? 0 : found->second)
                        << "global cell " << tileX * side + x << "," << tileY * side + y;
                }
            }
        const auto &s = descriptor->statistics;
        EXPECT_EQ(descriptor->metadata.sourceMeasurements, total);
        EXPECT_EQ(s.scanned, total); EXPECT_EQ(s.eventsWritten, total); EXPECT_EQ(s.eventsRead, total);
        EXPECT_EQ(s.accumulated, total); EXPECT_EQ(s.tileTotal, total); EXPECT_EQ(s.density.total, total);
        EXPECT_EQ(s.scanAllCalls, 1); EXPECT_EQ(s.spatialQueries, 0);
        EXPECT_EQ(s.visitedGroups, s.occupiedGroups);
    }
};

TEST_F(DensityOperation, ReceiverIsRequiredBeforeWorkerAdmission)
{
    initial();
    ASSERT_EQ(controller.setDensityReceiver({}).outcome, CommandOutcome::Applied);
    const auto lastId = controller.operation().id;
    EXPECT_EQ(controller.buildDensity().outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.operation().id, lastId);
    EXPECT_FALSE(controller.operation().active);
    EXPECT_FALSE(OperationTestAccess::runner(controller).busy());
    EXPECT_EQ(receiver->prepares, 0);
}

TEST_F(DensityOperation, SingletonAndDuplicateCountsAreReadByIndependentReader)
{
    initial("2.5 -3 0\n2.5 -3 0\n2.5 -3 0\n2.5 -3 0\n");
    ASSERT_TRUE(QFile::remove(source));
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
    ASSERT_EQ(receiver->owner->entries.size(), 1);
    verify(receiver->owner->entries.front().descriptor, 1, 1, {{{0, 0}, 4}});
    EXPECT_EQ(receiver->owner->entries.front().descriptor->snapshot->projectId, controller.project()->projectId());
}

TEST_F(DensityOperation, MultiGroupPartialTilesKeepLiteralCountsAndSource)
{
    initial("0 0 0\n0 0 0\n255 255 0\n256 256 0\n256 256 0\n511 511 0\n"
            "512 512 0\n512 512 0\n512 512 0\n1025 771 0\n1025 771 0\n");
    const auto original = digest(cachePath());
    DensityBuildRequest request;
    request.eventCapacity = 3;
    ASSERT_EQ(controller.buildDensity(request).outcome, CommandOutcome::Applied);
    accepted();
    verify(receiver->owner->entries.back().descriptor, 1025, 771,
        {{{0, 0}, 2}, {{255, 255}, 1}, {{256, 256}, 2}, {{511, 511}, 1}, {{512, 512}, 3}, {{1024, 770}, 2}});
    EXPECT_EQ(receiver->owner->entries.back().descriptor->statistics.occupiedGroups, 3);
    EXPECT_EQ(digest(cachePath()), original);
}

TEST_F(DensityOperation, RepeatBuildRetainsBothDatasetsEditsAndSelectsNewResult)
{
    initial();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
    const auto first = receiver->owner->entries.front().descriptor;
    const auto original = digest(first->completedPath);
    receiver->owner->entries.front().roughEdit = 73;
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    receiver->owner->selected = first->metadata.id;
    accepted();
    ASSERT_EQ(receiver->owner->entries.size(), 2);
    EXPECT_EQ(receiver->owner->commits, 2);
    const auto second = receiver->owner->entries.back().descriptor;
    EXPECT_NE(first->metadata.id, second->metadata.id);
    EXPECT_NE(first->completedPath, second->completedPath);
    EXPECT_EQ(receiver->owner->entries.front().roughEdit, 73);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    // The second version was selected at admission. A later choice of the real
    // first version must still be replaced by the newly accepted third version.
    receiver->owner->selected = first->metadata.id;
    accepted();
    ASSERT_EQ(receiver->owner->entries.size(), 3);
    EXPECT_EQ(receiver->owner->commits, 3);
    EXPECT_NE(receiver->owner->entries.back().descriptor->metadata.id, first->metadata.id);
    EXPECT_NE(receiver->owner->entries.back().descriptor->metadata.id, second->metadata.id);
    EXPECT_EQ(digest(first->completedPath), original);
    verify(first, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
    verify(second, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
}

TEST_F(DensityOperation, FutureCellChangesWhileScanIsRunningOnlyAffectNextBuild)
{
    initial();
    ASSERT_EQ(controller.setCell(0.1).outcome, CommandOutcome::Applied);
    Event checkpoint, resume;
    bool stopped = false;
    ASSERT_EQ(DensityOperationTestAccess::start(controller, [&](density::BuildPhase phase) {
        if (phase == density::BuildPhase::Scan && !std::exchange(stopped, true)) {
            checkpoint.set(); resume.wait();
        }
    }).outcome, CommandOutcome::Applied);
    checkpoint.wait();
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_FALSE(controller.operation().publicationRevoked);
    EXPECT_EQ(controller.project()->inputRevision(), 2);
    resume.set();
    accepted();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_DOUBLE_EQ(receiver->owner->entries.front().descriptor->metadata.mapping.cell(), 0.1);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
    EXPECT_DOUBLE_EQ(receiver->owner->entries.back().descriptor->metadata.mapping.cell(), 0.5);
}

TEST_F(DensityOperation, FutureCellUndoRedoAfterReadyPreservesSnapshotHistoryAndDirty)
{
    initial();
    ASSERT_EQ(controller.setCell(0.1).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto descriptor = pending();
    ASSERT_TRUE(descriptor);
    EXPECT_DOUBLE_EQ(descriptor->snapshot->cell, 0.1);
    const auto startRevision = controller.project()->inputRevision();
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.setCell(0.25).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(controller.project()->inputRevision(), startRevision + 5);
    EXPECT_FALSE(controller.operation().publicationRevoked);
    EXPECT_TRUE(controller.canRedo());
    accepted();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_TRUE(controller.isDirty());
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 3);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), 2);
    verify(descriptor, 20, 20, {{{0, 0}, 2}, {{19, 19}, 1}});
}

TEST_F(DensityOperation, FutureCellChangeDuringPrepareStillCommitsWithoutReplacingSession)
{
    initial();
    const auto identity = controller.project()->runtimeIdentity();
    const auto projectId = controller.project()->projectId();
    receiver->duringPrepare = [&](const Descriptor &) {
        EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.redo().outcome, CommandOutcome::Applied);
    };
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
    EXPECT_EQ(controller.project()->runtimeIdentity(), identity);
    EXPECT_EQ(controller.project()->projectId(), projectId);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(controller.project()->inputRevision(), 3);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
    EXPECT_DOUBLE_EQ(receiver->owner->entries.front().descriptor->snapshot->cell, 1);
}

TEST_F(DensityOperation, ExplicitInvalidationCannotBeUndoneByCellABA)
{
    initial();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto path = pending()->completedPath;
    OperationTestAccess::invalidate(controller);
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_FALSE(QFileInfo::exists(path));
}

TEST_F(DensityOperation, ReceiverReplacementAndABARevokeOriginalPublication)
{
    initial();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto path = pending()->completedPath;
    auto other = std::make_shared<Receiver>();
    EXPECT_EQ(controller.setDensityReceiver(other).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.setDensityReceiver(receiver).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_EQ(other->prepares, 0);
    EXPECT_FALSE(QFileInfo::exists(path));
}

TEST_F(DensityOperation, EpochChangeRejectsEvenIdenticalSourceAndProject)
{
    initial();
    const auto projectId = controller.project()->projectId();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    DensityOperationTestAccess::advanceEpoch(controller);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.project()->projectId(), projectId);
    EXPECT_EQ(controller.operation().status, OperationStatus::Discarded);
    EXPECT_EQ(receiver->owner->commits, 0);
}

TEST_F(DensityOperation, SourceBinaryBoundsChangeRejectsReady)
{
    initial("-0 0 0\n2 2 0\n");
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    auto sourceSnapshot = *controller.project()->dataset();
    ASSERT_TRUE(std::signbit(sourceSnapshot.minimum.x));
    sourceSnapshot.minimum.x = 0;
    ProjectControllerTestAccess::setDataset(controller, std::move(sourceSnapshot));
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(receiver->owner->commits, 0);
}

TEST_F(DensityOperation, ReceiverRefusalAndExceptionsPreservePreviousCatalogue)
{
    initial();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
    const auto previous = receiver->owner->entries.front().descriptor;
    for (int mode = 0; mode != 4; ++mode) {
        receiver->refuse = mode == 0;
        receiver->duringPrepare = [mode](const Descriptor &) {
            if (mode == 1) throw std::bad_alloc();
            if (mode == 2) throw std::runtime_error("receiver preparation failure");
            if (mode == 3) throw std::invalid_argument("receiver validation failure");
        };
        ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
        awaitReady();
        const auto path = pending()->completedPath;
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_EQ(controller.operation().status, mode == 0 ? OperationStatus::Discarded : OperationStatus::Failed);
        if (mode != 0) {
            ASSERT_TRUE(controller.operation().failure);
            EXPECT_EQ(controller.operation().failure->reason, mode == 1 ? FailureReason::OutOfMemory
                : FailureReason::CandidatePreparationFailed);
        }
        EXPECT_EQ(receiver->owner->commits, 1);
        EXPECT_EQ(receiver->owner->entries.size(), 1);
        EXPECT_EQ(receiver->owner->selected, previous->metadata.id);
        EXPECT_TRUE(QFileInfo::exists(previous->completedPath));
        EXPECT_FALSE(QFileInfo::exists(path));
    }
}

TEST_F(DensityOperation, CandidateForAnotherDescriptorIsDiscarded)
{
    initial();
    receiver->invalidCandidate = true;
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto path = pending()->completedPath;
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_FALSE(QFileInfo::exists(path));
}

class DensityPhaseCancellation : public DensityOperation, public testing::WithParamInterface<density::BuildPhase> {};
TEST_P(DensityPhaseCancellation, CooperativeStopCleansRealBuilderResources)
{
    initial();
    Event checkpoint, resume;
    bool stopped = false;
    ASSERT_EQ(DensityOperationTestAccess::start(controller, [&](density::BuildPhase phase) {
        if (phase == GetParam() && !std::exchange(stopped, true)) {
            checkpoint.set(); resume.wait();
        }
    }).outcome, CommandOutcome::Applied);
    checkpoint.wait();
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.buildDensity().failure->category, CommandError::Busy);
    resume.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_EQ(QDir(root).entryList({QStringLiteral("density-*")}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}
INSTANTIATE_TEST_SUITE_P(Checkpoints, DensityPhaseCancellation,
    testing::Values(density::BuildPhase::Scan, density::BuildPhase::Accumulation,
                    density::BuildPhase::Finish, density::BuildPhase::Complete));

TEST_F(DensityOperation, ThrowingProgressBridgeIsFailureDespiteLateCancellation)
{
    initial();
    ASSERT_EQ(DensityOperationTestAccess::start(controller, [](density::BuildPhase phase) {
        if (phase == density::BuildPhase::Scan) throw std::runtime_error("bridge fault");
    }).outcome, CommandOutcome::Applied);
    awaitReady();
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    ASSERT_TRUE(controller.operation().failure);
    ASSERT_TRUE(controller.operation().failure->densityError);
    EXPECT_EQ(controller.operation().failure->densityError->code, density::BuildErrorCode::DensityFailure);
    EXPECT_EQ(controller.operation().failure->densityError->phase, density::BuildPhase::Scan);
    EXPECT_EQ(receiver->owner->commits, 0);
}

TEST_F(DensityOperation, ReadyAllocationFailureCleansCompletedDatasetAndDoesNotWaitForever)
{
    initial();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
    const auto previous = receiver->owner->entries.front().descriptor;
    ASSERT_EQ(DensityOperationTestAccess::start(controller, {}, {}, [] { throw std::bad_alloc(); }).outcome,
        CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    ASSERT_TRUE(controller.operation().failure);
    EXPECT_EQ(controller.operation().failure->reason, FailureReason::OutOfMemory);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(receiver->owner->commits, 1);
    EXPECT_EQ(receiver->owner->selected, previous->metadata.id);
    EXPECT_TRUE(QFileInfo::exists(previous->completedPath));
    EXPECT_EQ(QDir(root).entryList({QStringLiteral("density-*")}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
}

TEST_F(DensityOperation, CloseOpenWithSameSavedProjectAndCacheRejectsOldReady)
{
    initial();
    const auto manifest = area.path() + QStringLiteral("/saved/project.pc2dproj");
    ASSERT_EQ(controller.saveProject({manifest}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    DensityOperationTestAccess::stopAutosave(controller);
    const auto projectId = controller.project()->projectId();
    const auto cacheId = controller.project()->dataset()->cacheId;
    const auto identity = controller.project()->runtimeIdentity();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto oldOperation = controller.operation().id;
    const auto oldPath = pending()->completedPath;
    ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().decision, PublicationDecision::Discard);
    ASSERT_EQ(controller.openProject({manifest}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    ASSERT_NE(controller.project(), nullptr);
    EXPECT_EQ(controller.project()->projectId(), projectId);
    EXPECT_EQ(controller.project()->dataset()->cacheId, cacheId);
    EXPECT_GT(controller.project()->runtimeIdentity(), identity);
    OperationTestAccess::ready(controller, oldOperation);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_FALSE(QFileInfo::exists(oldPath));
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
}

TEST_F(DensityOperation, AutosavePreemptionReturnsBusyWithoutQueuedBuild)
{
    initial();
    Event entered, resume;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
        [&](std::function<void()> entry) {
            return std::unique_ptr<QThread>(QThread::create([&, entry = std::move(entry)] {
                entered.set(); resume.wait(); entry();
            }));
        });
    ASSERT_EQ(controller.autosave().outcome, CommandOutcome::Applied);
    entered.wait();
    const auto autosaveId = controller.operation().id;
    EXPECT_EQ(controller.buildDensity().failure->category, CommandError::Busy);
    EXPECT_EQ(controller.operation().id, autosaveId);
    EXPECT_EQ(controller.operation().kind, OperationKind::Autosave);
    EXPECT_TRUE(controller.operation().cancellationRequested);
    resume.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_FALSE(OperationTestAccess::runner(controller).busy());
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto buildId = controller.operation().id;
    EXPECT_EQ(controller.autosave().failure->category, CommandError::Busy);
    EXPECT_EQ(controller.saveProject().failure->category, CommandError::Busy);
    EXPECT_EQ(controller.importAscXyz({source, limits, true}).failure->category, CommandError::Busy);
    EXPECT_EQ(controller.openProject({area.path() + "/missing.pc2dproj"}).failure->category, CommandError::Busy);
    EXPECT_EQ(controller.operation().id, buildId);
    accepted();
}

TEST_F(DensityOperation, AbsoluteUnicodeBindingsAreIndependentOfCurrentDirectory)
{
    initial();
    QTemporaryDir unrelated;
    ASSERT_TRUE(unrelated.isValid());
    struct RestoreCwd { QString path = QDir::currentPath(); ~RestoreCwd() { QDir::setCurrent(path); } } restore;
    Event entered, resume;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
        [&](std::function<void()> entry) {
            return std::unique_ptr<QThread>(QThread::create([&, entry = std::move(entry)] {
                entered.set(); resume.wait(); entry();
            }));
        });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    entered.wait();
    EXPECT_TRUE(QDir::setCurrent(unrelated.path()));
    resume.set();
    accepted();
    verify(receiver->owner->entries.front().descriptor, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
    EXPECT_TRUE(QDir(unrelated.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}

TEST_F(DensityOperation, LockedOwnFileGivesSecondaryCleanupWarning)
{
    initial();
    HANDLE locked = INVALID_HANDLE_VALUE;
    receiver->duringPrepare = [&](const Descriptor &descriptor) {
        locked = CreateFileW(reinterpret_cast<LPCWSTR>(descriptor->completedPath.utf16()),
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        EXPECT_NE(locked, INVALID_HANDLE_VALUE);
    };
    receiver->refuse = true;
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto descriptor = pending();
    ASSERT_TRUE(finish(controller));
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    EXPECT_EQ(controller.operation().status, OperationStatus::Discarded);
    EXPECT_TRUE(controller.operation().cleanupWarning);
    ASSERT_TRUE(controller.operation().densityCleanupWarning);
    EXPECT_EQ(controller.operation().densityCleanupWarning->phase, density::BuildPhase::Cleanup);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_TRUE(QFileInfo::exists(descriptor->completedPath));
    EXPECT_TRUE(QFile::remove(descriptor->completedPath));
    EXPECT_TRUE(QDir().rmdir(descriptor->snapshot->outputScope));
}

TEST_F(DensityOperation, DiscardNeverRecursivelyDeletesForeignChildren)
{
    initial();
    QString foreign;
    receiver->duringPrepare = [&](const Descriptor &descriptor) {
        foreign = descriptor->snapshot->outputScope + QStringLiteral("/foreign.txt");
        QFile file(foreign);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("keep"), 4);
    };
    receiver->refuse = true;
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto descriptor = pending();
    ASSERT_TRUE(finish(controller));
    EXPECT_TRUE(controller.operation().cleanupWarning);
    EXPECT_TRUE(QFileInfo::exists(foreign));
    EXPECT_FALSE(QFileInfo::exists(descriptor->completedPath));
    EXPECT_EQ(receiver->owner->commits, 0);
}

class DensityMalformedProduct : public DensityOperation, public testing::WithParamInterface<int> {};
TEST_P(DensityMalformedProduct, ProductionProductCoherenceIsValidatedBeforeReady)
{
    initial();
    const auto original = digest(cachePath());
    QString outputPath;
    ASSERT_EQ(DensityOperationTestAccess::start(controller, {}, [&](WorkerOutcome &outcome) {
        if (outcome.status != WorkerStatus::Succeeded || !outcome.density)
            throw std::runtime_error("fixture production Density build failed");
        outputPath = outcome.density->ready->completedPath;
        if (GetParam() == 0) { outcome.density.reset(); return; }
        if (GetParam() == 1) { outcome.product = std::make_unique<VerificationProduct>(); return; }
        if (GetParam() == 2) { outcome.density->ready.reset(); return; }
        auto ready = std::make_shared<DensityReadyProduct>(*outcome.density->ready);
        switch (GetParam()) {
        case 3: ready->metadata.id = density::DatasetId::create(); break;
        case 4: ++ready->metadata.sourceMeasurements; break;
        case 5: {
            auto mapping = raster::Mapping::create({0, 0}, {2, 2}, 0.5, raster::ValidationPolicy{100});
            if (!mapping) throw std::runtime_error("fixture mapping");
            ready->metadata.mapping = mapping.value(); break;
        }
        case 6: ready->completedPath = cachePath(); break;
        case 7: ready->metadata.tileSide = 512; break;
        case 8: ++ready->statistics.scanned; break;
        case 9: ++ready->statistics.sourceRead.returnedRecords; break;
        }
        outcome.density->ready = std::move(ready);
    }).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    ASSERT_TRUE(controller.operation().failure);
    EXPECT_EQ(controller.operation().failure->reason, FailureReason::InvalidProduct);
    EXPECT_EQ(receiver->prepares, 0);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_FALSE(QFileInfo::exists(outputPath));
    EXPECT_EQ(digest(cachePath()), original);
}
INSTANTIATE_TEST_SUITE_P(InvalidProducts, DensityMalformedProduct, testing::Range(0, 10));

TEST_F(DensityOperation, CancellationBeforeEntryRetainsBusyUntilWorkerJoin)
{
    initial();
    Event entered, release;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller),
        [&](std::function<void()> entry) {
            return std::unique_ptr<QThread>(QThread::create([&, entry = std::move(entry)] {
                entered.set(); release.wait(); entry();
            }));
        });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    entered.wait();
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.buildDensity().failure->category, CommandError::Busy);
    EXPECT_TRUE(controller.operation().active);
    release.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    EXPECT_EQ(receiver->prepares, 0);
    EXPECT_EQ(receiver->owner->commits, 0);
}

TEST_F(DensityOperation, CancellationAfterReadyAndInsidePrepareNeverCommits)
{
    initial();
    for (bool inside : {false, true}) {
        receiver->duringPrepare = inside ? std::function<void(const Descriptor &)>([&](const Descriptor &descriptor) {
            EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
            // The lease keeps the immutable descriptor and worker D alive here.
            EXPECT_TRUE(QFileInfo::exists(descriptor->completedPath));
        }) : std::function<void(const Descriptor &)>{};
        ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
        awaitReady();
        const auto path = pending()->completedPath;
        if (!inside) EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller));
        EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
        EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Succeeded);
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
        EXPECT_EQ(receiver->owner->commits, 0);
        EXPECT_FALSE(QFileInfo::exists(path));
    }
}

TEST_F(DensityOperation, ReentrantCloseAndShutdownPrepareKeepsBorrowedDescriptorAlive)
{
    initial();
    receiver->duringPrepare = [&](const Descriptor &descriptor) {
        const auto id = descriptor->metadata.id;
        EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.project(), nullptr);
        EXPECT_TRUE(QFileInfo::exists(descriptor->completedPath));
        EXPECT_EQ(descriptor->metadata.id, id);
        EXPECT_EQ(receiver->owner->commits, 0);
    };
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto path = pending()->completedPath;
    ASSERT_TRUE(spinUntil([&] {
        OperationTestAccess::poll(controller);
        return controller.shutdownState() == ShutdownState::Ready;
    }));
    EXPECT_FALSE(controller.operation().active);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_FALSE(QFileInfo::exists(path));
}

TEST_F(DensityOperation, ImmediateCloseAfterAcceptKeepsFileBeforeWorkerHandoff)
{
    initial();
    Event handoff, allowHandoff;
    OperationRunnerTestAccess::beforeOwnershipFinalization(OperationTestAccess::runner(controller),
        [&](PublicationDecision decision) {
            EXPECT_EQ(decision, PublicationDecision::Accept);
            handoff.set(); allowHandoff.wait();
        });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(spinUntil([&] {
        OperationTestAccess::poll(controller);
        return handoff.isSet();
    }));
    ASSERT_EQ(receiver->owner->entries.size(), 1);
    const auto acceptedDescriptor = receiver->owner->entries.front().descriptor;
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project(), nullptr);
    verify(acceptedDescriptor, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
    EXPECT_EQ(controller.buildDensity().failure->category, CommandError::Busy);
    allowHandoff.set();
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    verify(acceptedDescriptor, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
}

TEST_F(DensityOperation, ImmediateShutdownAfterAcceptWaitsForHandoffAndRetainsD)
{
    initial();
    Event handoff, allowHandoff;
    OperationRunnerTestAccess::beforeOwnershipFinalization(OperationTestAccess::runner(controller),
        [&](PublicationDecision decision) {
            EXPECT_EQ(decision, PublicationDecision::Accept);
            handoff.set(); allowHandoff.wait();
        });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return handoff.isSet(); }));
    ASSERT_EQ(receiver->owner->entries.size(), 1);
    const auto descriptor = receiver->owner->entries.front().descriptor;
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    EXPECT_TRUE(controller.operation().active);
    verify(descriptor, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
    allowHandoff.set();
    ASSERT_TRUE(spinUntil([&] {
        OperationTestAccess::poll(controller);
        return controller.shutdownState() == ShutdownState::Ready;
    }));
    EXPECT_FALSE(controller.operation().active);
    verify(descriptor, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
}

class DensityProgress : public DensityOperation, public testing::WithParamInterface<density::BuildPhase> {};
TEST_P(DensityProgress, PhaseCountersArePolledWithoutTerminalSuccess)
{
    initial();
    Event checkpoint, resume;
    bool stopped = false;
    ASSERT_EQ(DensityOperationTestAccess::start(controller, [&](density::BuildPhase phase) {
        if (phase == GetParam() && !std::exchange(stopped, true)) {
            checkpoint.set(); resume.wait();
        }
    }).outcome, CommandOutcome::Applied);
    checkpoint.wait();
    OperationTestAccess::poll(controller);
    const auto &operation = controller.operation();
    const auto expectedPhase = GetParam() == density::BuildPhase::Scan ? OperationPhase::Scan
        : GetParam() == density::BuildPhase::Accumulation ? OperationPhase::Accumulation : OperationPhase::Finalizing;
    EXPECT_EQ(operation.progress.phase, expectedPhase);
    if (GetParam() == density::BuildPhase::Finish) EXPECT_FALSE(operation.progress.total);
    else EXPECT_EQ(operation.progress.total, 3);
    EXPECT_FALSE(operation.workerStatus);
    EXPECT_EQ(operation.decision, PublicationDecision::Pending);
    EXPECT_TRUE(operation.active);
    bool eventDelivered = false;
    QTimer::singleShot(0, [&] { eventDelivered = true; });
    EXPECT_TRUE(spinUntil([&] { return eventDelivered; }));
    EXPECT_EQ(receiver->owner->commits, 0);
    resume.set();
    accepted();
}
INSTANTIATE_TEST_SUITE_P(Counters, DensityProgress, testing::Values(
    density::BuildPhase::Scan, density::BuildPhase::Accumulation, density::BuildPhase::Finish));

TEST_F(DensityOperation, SmallProductionPipelineRecordsLatencyEventsAndOwnership)
{
    initial();
    QElapsedTimer timer;
    unsigned progressNotifications = 0;
    const auto connection = QObject::connect(&OperationTestAccess::runner(controller),
        &OperationRunner::progressAvailable, [&](OperationId, OperationProgress) { ++progressNotifications; });
    receiver->owner->clock = &timer;
    timer.start();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    const auto admittedAt = timer.nsecsElapsed();
    accepted();
    const auto joinedAt = timer.nsecsElapsed();
    receiver->owner->clock = nullptr;
    QObject::disconnect(connection);
    ASSERT_EQ(receiver->owner->commits, 1);
    ASSERT_EQ(receiver->owner->entries.size(), 1);
    const auto descriptor = receiver->owner->entries.front().descriptor;
    EXPECT_GE(receiver->owner->preparedAt, admittedAt);
    EXPECT_GE(receiver->owner->committedAt, receiver->owner->preparedAt);
    EXPECT_GE(joinedAt, receiver->owner->committedAt);
    verify(descriptor, 2, 2, {{{0, 0}, 2}, {{1, 1}, 1}});
    RecordProperty("admission_ns", std::to_string(admittedAt));
    RecordProperty("prepare_ns", std::to_string(receiver->owner->preparedAt));
    RecordProperty("commit_ns", std::to_string(receiver->owner->committedAt));
    RecordProperty("native_join_terminal_ns", std::to_string(joinedAt));
    RecordProperty("progress_notifications", std::to_string(progressNotifications));
    RecordProperty("source_points", std::to_string(descriptor->metadata.sourceMeasurements));
    RecordProperty("scan_all_calls", std::to_string(descriptor->statistics.scanAllCalls));
    RecordProperty("events_written", std::to_string(descriptor->statistics.eventsWritten));
    RecordProperty("events_read", std::to_string(descriptor->statistics.eventsRead));
    RecordProperty("output_bytes", std::to_string(descriptor->statistics.density.outputBytes));
    RecordProperty("working_owned_peak", std::to_string(descriptor->statistics.workingPeak));
    RecordProperty("retained_descriptors", std::to_string(receiver->owner->entries.size()));
}

TEST_F(DensityOperation, DuplicateAndStaleReadyNeverCommitTwice)
{
    initial();
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    awaitReady();
    const auto id = controller.operation().id;
    OperationTestAccess::ready(controller, id + 1);
    EXPECT_EQ(receiver->owner->commits, 0);
    OperationTestAccess::ready(controller, id);
    OperationTestAccess::ready(controller, id);
    accepted();
    EXPECT_EQ(receiver->owner->commits, 1);
    OperationTestAccess::ready(controller, id);
    EXPECT_EQ(receiver->owner->commits, 1);
}

TEST_F(DensityOperation, ExplicitResourceRefusalPreservesStructuredBuildFailure)
{
    initial();
    const auto original = digest(cachePath());
    DensityBuildRequest request;
    request.resources.workingBytes = 1;
    ASSERT_EQ(controller.buildDensity(request).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    ASSERT_TRUE(controller.operation().failure);
    ASSERT_TRUE(controller.operation().failure->densityError);
    EXPECT_EQ(controller.operation().failure->densityError->code, density::BuildErrorCode::ResourceRefused);
    EXPECT_GT(controller.operation().failure->densityError->required, controller.operation().failure->densityError->budget);
    EXPECT_EQ(receiver->owner->commits, 0);
    EXPECT_EQ(digest(cachePath()), original);
}

TEST_F(DensityOperation, WrongThreadAndNotificationReentryAreRejected)
{
    initial();
    CommandResult other = CommandResult::noChange();
    std::thread thread([&] { other = controller.buildDensity(); });
    thread.join();
    ASSERT_TRUE(other.failure);
    EXPECT_EQ(other.failure->detail, ErrorDetail::WrongThread);
    unsigned notifications = 0;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        ++notifications;
        EXPECT_EQ(controller.buildDensity().failure->detail, ErrorDetail::ReentrantCommand);
    });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    accepted();
    EXPECT_EQ(notifications, 1);
}

TEST_F(DensityOperation, OwnerEventLoopRemainsResponsiveWhileRealWorkerIsHeld)
{
    initial();
    Event entered, allow;
    OperationRunnerTestAccess::configure(OperationTestAccess::runner(controller), {}, false,
        [&] { entered.set(); allow.wait(); });
    ASSERT_EQ(controller.buildDensity().outcome, CommandOutcome::Applied);
    entered.wait();
    bool delivered = false;
    QTimer::singleShot(0, [&] { delivered = true; });
    ASSERT_TRUE(spinUntil([&] { OperationTestAccess::poll(controller); return delivered; }));
    EXPECT_TRUE(controller.operation().active);
    EXPECT_EQ(controller.operation().workerStatus, WorkerStatus::Succeeded);
    EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
    allow.set();
    accepted();
}

} // namespace
