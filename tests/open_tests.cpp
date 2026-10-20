#include "operation_test_support.h"
#include "save_test_access.h"
#include "project_test_access.h"
#include "cache/binary_codec.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStorageInfo>
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <limits>

#define NOMINMAX
#include <Windows.h>
#include <sddl.h>

using namespace cadcontour;
using namespace cadcontour::test;
using namespace cadcontour::persistence;

namespace cadcontour {
class OpenTestAccess {
public:
    static void preparation(ProjectController &c, std::function<void()> hook) { c.prepareOpenHook_ = std::move(hook); }
    static void epoch(ProjectController &c) { ++c.contextEpoch_; }
};
}
namespace {
constexpr ManifestLimits manifestLimits{4ULL << 20, 4096, 32};
class Open : public testing::Test {
protected:
    QTemporaryDir area;
    ProjectController controller{area.path() + "/import"};
    QString source = area.path() + QStringLiteral("/исходные точки.xyz");
    QString target = area.path() + QStringLiteral("/Проект А/Деталь.pc2dproj");
    QString cachePath;
    ProjectSnapshot original;
    cache::ResourceLimits limits{8ULL << 20, 64ULL << 20, 8};
    static QByteArray bytes(const QString &path)
    { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
    static void write(const QString &path, const QByteArray &data)
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); ASSERT_EQ(f.write(data), data.size()); }
    void SetUp() override
    {
        ASSERT_TRUE(area.isValid());
        write(source, "-0 2 0\n3 -4 0\n");
        ASSERT_EQ(controller.importAscXyz({source, limits}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller)); ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        ASSERT_EQ(controller.setCell(std::nextafter(0.1, 1.0)).outcome, CommandOutcome::Applied);
        ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller)); ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        original = controller.savedProject()->snapshot;
        cachePath = QString::fromStdWString(controller.project()->dataset()->cachePath().wstring());
    }
    void open(bool confirmed = true)
    {
        OpenRequest request{target}; request.replacementConfirmed = confirmed;
        ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller)); ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    }
    void ready()
    {
        const auto c = OperationRunnerTestAccess::context(OperationTestAccess::runner(controller));
        ASSERT_TRUE(c); ASSERT_TRUE(waitContext(c, true));
    }
    void partial(CacheAccess access)
    {
        open();
        ASSERT_TRUE(controller.project()); ASSERT_TRUE(controller.savedProject());
        EXPECT_EQ(controller.pointCacheAvailability()->access, access);
        EXPECT_FALSE(controller.savedProject()->datasetIdentity);
        EXPECT_EQ(controller.project()->projectId(), original.projectId);
        EXPECT_EQ(controller.project()->dataset()->cacheId, original.pointCacheId);
        EXPECT_EQ(controller.project()->dataset()->pointCount, original.datasets[0].metadata.pointCount);
        EXPECT_DOUBLE_EQ(controller.project()->cell(), original.cell);
        EXPECT_FALSE(controller.isDirty()); EXPECT_FALSE(controller.canUndo()); EXPECT_FALSE(controller.canRedo());
        EXPECT_EQ(controller.setCell(0.5).failure->detail, ErrorDetail::DatasetUnavailable);
        EXPECT_EQ(controller.saveProject().failure->detail, ErrorDetail::DatasetUnavailable);
        EXPECT_EQ(controller.saveProjectAs({area.path() + "/other/p.pc2dproj"}).failure->detail, ErrorDetail::DatasetUnavailable);
        EXPECT_EQ(OperationTestAccess::start(controller, [](const auto &s, const auto &) { return success(s); })
                  .failure->detail, ErrorDetail::DatasetUnavailable);
    }
    void mutateManifest(const std::function<void(QJsonObject &)> &mutation)
    {
        auto o = QJsonDocument::fromJson(bytes(target)).object(); mutation(o);
        write(target, QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    void corrupt(qint64 offset)
    {
        QFile file(cachePath); ASSERT_TRUE(file.open(QIODevice::ReadWrite)); ASSERT_TRUE(file.seek(offset));
        const auto b = file.read(1); ASSERT_EQ(b.size(), 1); ASSERT_TRUE(file.seek(offset));
        ASSERT_EQ(file.write(QByteArray(1, char(b[0] ^ 0x40))), 1);
    }
    void recheck(CacheAccess expected)
    {
        ASSERT_EQ(controller.recheckPointCache(limits).outcome, CommandOutcome::Applied);
        ASSERT_TRUE(finish(controller)); ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
        EXPECT_EQ(controller.pointCacheAvailability()->access, expected);
    }
};

TEST_F(Open, RoundTripNewSessionExactCellSourceBaselineAndRepeatedSaveWithoutCopy)
{
    const auto oldRuntime = controller.project()->runtimeIdentity();
    const auto dataset = *controller.project()->dataset();
    const auto manifestBytes = bytes(target); const auto cacheBytes = bytes(cachePath);
    ASSERT_TRUE(QFile::remove(source));
    ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    open(false);
    EXPECT_NE(controller.project()->runtimeIdentity(), oldRuntime);
    EXPECT_EQ(controller.project()->inputRevision(), 0);
    EXPECT_EQ(controller.project()->projectId(), original.projectId);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(controller.project()->cell()), std::bit_cast<std::uint64_t>(original.cell));
    const auto &d = *controller.project()->dataset();
    EXPECT_EQ(d.sourcePath, dataset.sourcePath); EXPECT_EQ(d.sourceBytes, dataset.sourceBytes);
    EXPECT_EQ(d.sourceLines, dataset.sourceLines); EXPECT_EQ(d.pointCount, dataset.pointCount);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(d.minimum.x), std::bit_cast<std::uint64_t>(dataset.minimum.x));
    EXPECT_DOUBLE_EQ(d.minimum.y, -4); EXPECT_DOUBLE_EQ(d.maximum.x, 3); EXPECT_DOUBLE_EQ(d.maximum.y, 2);
    EXPECT_EQ(d.hasNonZeroZ, dataset.hasNonZeroZ); EXPECT_EQ(d.cacheId, dataset.cacheId);
    EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, original.snapshotId);
    EXPECT_EQ(controller.savedProject()->manifestDigest, QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256));
    EXPECT_TRUE(controller.savedProject()->datasetIdentity); EXPECT_FALSE(controller.isDirty());
    EXPECT_FALSE(controller.canUndo()); EXPECT_FALSE(controller.canRedo());
    EXPECT_EQ(bytes(target), manifestBytes); EXPECT_EQ(bytes(cachePath), cacheBytes);
    ASSERT_EQ(controller.setCell(0.25).outcome, CommandOutcome::Applied); EXPECT_TRUE(controller.isDirty());
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_FALSE(controller.isDirty());
    ASSERT_EQ(controller.redo().outcome, CommandOutcome::Applied); EXPECT_TRUE(controller.isDirty());
    SaveTestAccess::configure(controller, [](SavePhase p) {
        EXPECT_NE(p, SavePhase::CopyRead); EXPECT_NE(p, SavePhase::CopyWrite); EXPECT_NE(p, SavePhase::CopyVerify);
    });
    const auto identity = controller.savedProject()->datasetIdentity;
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(controller.savedProject()->datasetIdentity, identity);
    ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied); open(false);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.25); EXPECT_FALSE(controller.isDirty());
    EXPECT_FALSE(controller.canUndo()); EXPECT_EQ(bytes(cachePath), cacheBytes);
}

TEST_F(Open, MovedDirectoryUsesSelectedRootAndRetainsDatasetAfterClose)
{
    ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    const auto old = QFileInfo(target).absolutePath();
    const auto moved = area.path() + QStringLiteral("/перенесённый каталог");
    ASSERT_TRUE(QDir().rename(old, moved));
    target = QDir(moved).filePath(QFileInfo(target).fileName()); open(false);
    EXPECT_EQ(controller.project()->dataset()->location->storageDirectory, std::filesystem::path(moved.toStdWString()));
    const auto path = controller.project()->dataset()->cachePath();
    EXPECT_TRUE(QFileInfo::exists(QString::fromStdWString(path.wstring())));
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_TRUE(QFileInfo::exists(QString::fromStdWString(path.wstring())));
}

TEST_F(Open, CrossVolumeDirectoryTransfer)
{
    QTemporaryDir destination(QString::fromUtf8(OPEN_TEST_WORKSPACE) + "/open-volume-XXXXXX");
    ASSERT_TRUE(destination.isValid());
    if (QStorageInfo(area.path()).device() == QStorageInfo(destination.path()).device())
        GTEST_SKIP() << "TEMP and build are on the same volume; rerun with TEMP on another volume";
    const auto manifestBytes = bytes(target); const auto cacheBytes = bytes(cachePath);
    ASSERT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    const auto movedManifest = destination.path() + "/moved.pc2dproj";
    const auto movedCache = destination.path() + '/' + original.datasets[0].relativePath;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(movedCache).absolutePath()));
    ASSERT_TRUE(QFile::copy(target, movedManifest)); ASSERT_TRUE(QFile::copy(cachePath, movedCache));
    ASSERT_TRUE(QFile::remove(target)); ASSERT_TRUE(QFile::remove(cachePath));
    target = movedManifest; open(false);
    EXPECT_EQ(bytes(target), manifestBytes); EXPECT_EQ(bytes(movedCache), cacheBytes);
    EXPECT_EQ(controller.project()->dataset()->cachePath(), std::filesystem::path(movedCache.toStdWString()));
}

TEST_F(Open, MissingThenRestoredAndRecheckedWithoutNewSessionOrDirty)
{
    const auto data = bytes(cachePath); ASSERT_TRUE(QFile::remove(cachePath));
    partial(CacheAccess::Missing);
    const auto stamp = controller.project()->operationStamp();
    const auto saved = controller.savedProject()->snapshot.snapshotId;
    recheck(CacheAccess::Missing); write(cachePath, data); recheck(CacheAccess::AvailableLimited);
    EXPECT_EQ(controller.project()->operationStamp(), stamp); EXPECT_FALSE(controller.isDirty());
    EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, saved); EXPECT_TRUE(controller.savedProject()->datasetIdentity);
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_TRUE(controller.isDirty()); EXPECT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    EXPECT_FALSE(controller.isDirty());
    EXPECT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
}

TEST_F(Open, AvailableFileDisappearsSaveRejectsThenRecheckBlocksEditsAndKeepsHistory)
{
    open(); ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    const auto stamp = controller.project()->operationStamp(); const auto manifestBytes = bytes(target);
    ASSERT_TRUE(QFile::remove(cachePath));
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_EQ(bytes(target), manifestBytes);
    recheck(CacheAccess::Missing); EXPECT_EQ(controller.project()->operationStamp(), stamp);
    EXPECT_TRUE(controller.isDirty()); EXPECT_TRUE(controller.canUndo());
    EXPECT_EQ(controller.undo().failure->detail, ErrorDetail::DatasetUnavailable);
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
}

class CacheDamage : public Open, public testing::WithParamInterface<int> {};
TEST_P(CacheDamage, LimitedChecksRestorePartialMetadata)
{
    const auto mode = GetParam();
    if (mode == 0) { QFile f(cachePath); ASSERT_TRUE(f.open(QIODevice::ReadWrite)); ASSERT_TRUE(f.resize(17)); }
    if (mode == 1) corrupt(0);
    if (mode == 2) corrupt(QFileInfo(cachePath).size() - 2);
    if (mode == 3) corrupt(cache::format::headerSize + cache::format::pageSize + 80);
    if (mode == 4) {
        auto data = bytes(cachePath); data.replace(8, 4, QByteArray::fromHex("02000000"));
        data.replace(2016, 32, QCryptographicHash::hash(data.first(2016), QCryptographicHash::Sha256));
        write(cachePath, data);
    }
    if (mode >= 5) {
        mutateManifest([mode](QJsonObject &o) {
            auto d = o["datasets"].toArray()[0].toObject();
            if (mode == 5) d["hasNonZeroZ"] = true;
            if (mode == 6) { auto b = d["bounds"].toObject(); b["maxX"] = "4"; d["bounds"] = b; }
            if (mode == 7) {
                const auto id = QUuid::createUuid();
                const auto idText = id.toString(QUuid::WithoutBraces);
                auto state = o["state"].toObject(); state["pointCacheId"] = idText; o["state"] = state;
                d["cacheId"] = idText;
                d["path"] = QString(d["path"].toString()).replace(d["path"].toString().section('/', -1), idText + ".pc2dcache");
            }
            if (mode == 8) d["pointCount"] = "1";
            o["datasets"] = QJsonArray{d};
        });
        if (mode == 8) original.datasets[0].metadata.pointCount = 1;
        if (mode == 7) {
            QFile file(target); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            auto parsed = readManifest(file, manifestLimits); ASSERT_TRUE(parsed); file.close();
            const auto newPath = QFileInfo(target).absolutePath() + '/' + parsed.value().datasets[0].relativePath;
            ASSERT_TRUE(QFile::rename(cachePath, newPath)); cachePath = newPath;
            original = parsed.value();
        }
    }
    partial(mode == 4 ? CacheAccess::Unsupported : CacheAccess::Corrupt);
    EXPECT_TRUE(controller.pointCacheAvailability()->cacheError);
}
INSTANTIATE_TEST_SUITE_P(EnvelopeAndMetadata, CacheDamage, testing::Range(0, 9));

TEST_F(Open, PointPageCorruptionIsNotReadUntilExplicitFullValidation)
{
    corrupt(cache::format::headerSize + 80);
    const auto before = bytes(cachePath); const auto manifestBytes = bytes(target);
    const auto entries = QDir(area.path()).entryList(QDir::AllEntries | QDir::Hidden);
    open(); EXPECT_EQ(controller.pointCacheAvailability()->access, CacheAccess::AvailableLimited);
    const auto reads = controller.pointCacheAvailability()->reads;
    EXPECT_EQ(reads.pointPages, 0); EXPECT_EQ(reads.indexPages, 1);
    EXPECT_EQ(reads.readBytes, 2 * cache::format::headerSize + cache::format::pageSize);
    EXPECT_EQ(reads.readOperations, 3); EXPECT_EQ(reads.returnedRecords, 0);
    EXPECT_EQ(entries, QDir(area.path()).entryList(QDir::AllEntries | QDir::Hidden));
    EXPECT_EQ(bytes(cachePath), before); EXPECT_EQ(bytes(target), manifestBytes);
    auto reader = cache::CacheReader::open(cachePath, limits); ASSERT_TRUE(reader);
    auto full = reader.value()->validate(limits, area.path());
    ASSERT_FALSE(full); EXPECT_EQ(full.error().code, cache::ErrorCode::CorruptCache);
    // Availability remains a limited observation, never a FullValidation claim.
    recheck(CacheAccess::AvailableLimited); EXPECT_EQ(controller.pointCacheAvailability()->reads.pointPages, 0);
}

TEST_F(Open, SharingViolationIsPartialAndRetryRecovers)
{
    const auto handle = CreateFileW(cachePath.toStdWString().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    partial(CacheAccess::FileIO); EXPECT_TRUE(controller.pointCacheAvailability()->pathError);
    ASSERT_TRUE(CloseHandle(handle)); recheck(CacheAccess::AvailableLimited);
}

TEST_F(Open, ReadPermissionDeniedIsPartialAndRetryRecovers)
{
    struct SecurityRestore {
        std::wstring path;
        QByteArray previous;
        ~SecurityRestore() { if (!previous.isEmpty()) SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(previous.data())); }
    } restore{cachePath.toStdWString(), {}};
    DWORD size = 0;
    GetFileSecurityW(restore.path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
    ASSERT_GT(size, 0); restore.previous.resize(size);
    ASSERT_TRUE(GetFileSecurityW(restore.path.c_str(), DACL_SECURITY_INFORMATION,
        reinterpret_cast<PSECURITY_DESCRIPTOR>(restore.previous.data()), size, &size));
    PSECURITY_DESCRIPTOR deny = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;GR;;;WD)(A;;GA;;;WD)",
        SDDL_REVISION_1, &deny, nullptr));
    const auto changed = SetFileSecurityW(restore.path.c_str(), DACL_SECURITY_INFORMATION, deny);
    LocalFree(deny); ASSERT_TRUE(changed);
    partial(CacheAccess::FileIO);
    ASSERT_TRUE(SetFileSecurityW(restore.path.c_str(), DACL_SECURITY_INFORMATION,
        reinterpret_cast<PSECURITY_DESCRIPTOR>(restore.previous.data())));
    restore.previous.clear(); recheck(CacheAccess::AvailableLimited);
}

class BadManifest : public Open, public testing::WithParamInterface<int> {};
TEST_P(BadManifest, FailurePreservesPreviousSessionBaselineHistoryAndBinding)
{
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.setCell(0.75).outcome, CommandOutcome::Applied);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied);
    const auto stamp = controller.project()->operationStamp();
    const auto binding = controller.project()->dataset()->location;
    const auto saved = controller.savedProject();
    const auto count = ProjectControllerTestAccess::count(controller);
    const auto index = ProjectControllerTestAccess::index(controller);
    const auto mode = GetParam();
    OpenRequest request{target}; request.replacementConfirmed = true;
    if (mode == 0) write(target, "{");
    else if (mode <= 9) mutateManifest([mode](QJsonObject &o) {
        if (mode == 1) o["version"] = 99;
        if (mode == 2) o.remove("source");
        if (mode == 3) o["parameters"] = 42;
        if (mode == 4) { auto s = o["state"].toObject(); s["pointCacheId"] = QUuid::createUuid().toString(QUuid::WithoutBraces); o["state"] = s; }
        if (mode >= 5) {
            const char *paths[] = {"../outside", "C:/outside", "//server/share/file", "C:relative", "data\\wrong"};
            auto d = o["datasets"].toArray()[0].toObject(); d["path"] = paths[mode - 5]; o["datasets"] = QJsonArray{d};
        }
    });
    if (mode == 10) request.limits.maxBytes = 1;
    if (mode == 11) request.limits.maxTokens = 1;
    if (mode == 12) request.limits.maxDepth = 1;
    if (mode == 13) ASSERT_TRUE(QFile::remove(target));
    if (mode == 14) request.readerLimits.memoryBytes = 1;
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed);
    ASSERT_TRUE(controller.operation().failure); EXPECT_TRUE(controller.operation().failure->openError);
    EXPECT_EQ(controller.project()->operationStamp(), stamp); EXPECT_EQ(controller.project()->dataset()->location, binding);
    EXPECT_EQ(controller.savedProject(), saved); EXPECT_TRUE(controller.isDirty());
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), count); EXPECT_EQ(ProjectControllerTestAccess::index(controller), index);
    EXPECT_TRUE(controller.canUndo()); EXPECT_TRUE(controller.canRedo());
}
INSTANTIATE_TEST_SUITE_P(InvalidInputs, BadManifest, testing::Range(0, 15));

TEST_F(Open, ManifestReadSharingFailureLeavesPreviousProject)
{
    const auto state = controller.project();
    const auto h = CreateFileW(target.toStdWString().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    OpenRequest request{target}; request.replacementConfirmed = true;
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_EQ(controller.project(), state);
    ASSERT_TRUE(CloseHandle(h));
}

TEST_F(Open, DatasetJunctionNeverEscapesProjectEvenForMissingLeaf)
{
    const auto dataDirectory = QFileInfo(target).absolutePath() + "/data";
    const auto outside = area.path() + "/outside";
    ASSERT_TRUE(QDir().rename(dataDirectory, outside));
    QProcess process; process.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(dataDirectory), QDir::toNativeSeparators(outside)});
    ASSERT_TRUE(process.waitForFinished()); ASSERT_EQ(process.exitCode(), 0);
    struct RemoveJunction { QString path; ~RemoveJunction() { RemoveDirectoryW(path.toStdWString().c_str()); } } guard{dataDirectory};
    partial(CacheAccess::UnsafePath); EXPECT_TRUE(controller.pointCacheAvailability()->pathError);
    const auto file = outside + '/' + original.datasets[0].relativePath.mid(5);
    ASSERT_TRUE(QFile::remove(file)); recheck(CacheAccess::UnsafePath);
}

TEST_F(Open, ReplacementConsentAndCapturedContextAreChecked)
{
    const auto current = controller.project();
    EXPECT_EQ(controller.openProject({target}).failure->detail, ErrorDetail::ReplacementNotConfirmed);
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.openProject({target}).failure->detail, ErrorDetail::ReplacementNotConfirmed);
    OpenRequest request{target}; request.replacementConfirmed = true; request.expectedContext = controller.importContext();
    ASSERT_EQ(controller.setCell(0.75).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.openProject(request).failure->detail, ErrorDetail::ReplacementNotConfirmed);
    EXPECT_EQ(controller.project(), current);
    EXPECT_EQ(controller.openProject({"relative.pc2dproj"}).outcome, CommandOutcome::Rejected);
    open(); EXPECT_FALSE(controller.isDirty());
}

class OpenLifecycle : public Open, public testing::WithParamInterface<int> {};
TEST_P(OpenLifecycle, CancelInvalidateShutdownAndPreparationFailureNeverReplacePrevious)
{
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    const auto previous = controller.project(); const auto saved = controller.savedProject();
    const auto binding = previous->dataset()->location;
    OpenRequest request{target}; request.replacementConfirmed = true;
    const auto mode = GetParam();
    if (mode == 0) OpenTestAccess::preparation(controller, [] { throw std::bad_alloc(); });
    if (mode == 1) OpenTestAccess::preparation(controller, [] { throw std::runtime_error("candidate fault"); });
    if (mode == 2) OpenTestAccess::preparation(controller, [&] { controller.cancelOperation(); });
    if (mode == 3) OpenTestAccess::preparation(controller, [&] { controller.requestShutdown(); });
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); ready();
    if (mode == 4) EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied);
    if (mode == 5) EXPECT_EQ(controller.setCell(0.75).outcome, CommandOutcome::Applied);
    if (mode == 6) OpenTestAccess::epoch(controller);
    if (mode == 7) EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller)); EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
    EXPECT_EQ(controller.project(), previous); EXPECT_EQ(controller.savedProject(), saved);
    EXPECT_EQ(controller.project()->dataset()->location, binding); EXPECT_TRUE(controller.isDirty()); EXPECT_TRUE(controller.canUndo());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), mode == 5 ? 0.75 : 0.5);
    if (mode == 0) EXPECT_EQ(controller.operation().failure->reason, FailureReason::OutOfMemory);
    if (mode == 3 || mode == 7) EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
}
INSTANTIATE_TEST_SUITE_P(Atomicity, OpenLifecycle, testing::Range(0, 8));

TEST_F(Open, CancelBeforeWorkerEntryAndCloseBeforeReady)
{
    Event entered, proceed;
    auto &runner = OperationTestAccess::runner(controller);
    OperationRunnerTestAccess::configure(runner, [&](std::function<void()> run) {
        return std::unique_ptr<QThread>(QThread::create([&, run = std::move(run)] { entered.set(); proceed.wait(); run(); }));
    });
    OpenRequest request{target}; request.replacementConfirmed = true;
    const auto previous = controller.project();
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); entered.wait();
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied); proceed.set();
    ASSERT_TRUE(finish(controller)); EXPECT_EQ(controller.project(), previous);
    EXPECT_EQ(controller.operation().status, OperationStatus::Cancelled);
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); ready();
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.project(), nullptr); EXPECT_EQ(controller.operation().decision, PublicationDecision::Discard);
}

TEST_F(Open, BusyDuringImportSaveBeforeAndAfterPermissionAndOpen)
{
    OpenRequest request{target}; request.replacementConfirmed = true;
    ASSERT_EQ(controller.importAscXyz({source, limits, true}).outcome, CommandOutcome::Applied); ready();
    EXPECT_EQ(controller.openProject(request).failure->category, CommandError::Busy);
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ready();
    EXPECT_EQ(controller.openProject(request).failure->category, CommandError::Busy);
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    Event committing, proceed;
    SaveTestAccess::configure(controller, [&](SavePhase p) { if (p == SavePhase::BeforeCommit) { committing.set(); proceed.wait(); } });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ready();
    OperationTestAccess::ready(controller, controller.operation().id); committing.wait();
    EXPECT_EQ(controller.openProject(request).failure->category, CommandError::Busy);
    EXPECT_EQ(controller.closeProject().failure->category, CommandError::Busy);
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::NoChange); proceed.set(); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); ready();
    EXPECT_EQ(controller.openProject(request).failure->category, CommandError::Busy);
    ASSERT_TRUE(finish(controller));
}

TEST_F(Open, NotificationsAreConsistentReentryRejectedAndStaleCallbackIgnored)
{
    int notifications = 0;
    auto connection = QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        ++notifications; EXPECT_FALSE(controller.isDirty()); EXPECT_FALSE(controller.canUndo());
        EXPECT_EQ(controller.project()->projectId(), original.projectId);
        EXPECT_EQ(controller.openProject({target}).failure->detail, ErrorDetail::ReentrantCommand);
        OperationTestAccess::ready(controller, controller.operation().id);
    });
    open(); const auto id = controller.operation().id; const auto runtime = controller.project()->runtimeIdentity();
    EXPECT_EQ(notifications, 1); QObject::disconnect(connection);
    open(); EXPECT_NE(controller.project()->runtimeIdentity(), runtime);
    const auto state = controller.project(); OperationTestAccess::ready(controller, id);
    EXPECT_EQ(controller.project(), state); EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.openProject({target}).failure->detail, ErrorDetail::ShuttingDown);
}

TEST_F(Open, DestructorDrainsOpenWithoutQueuedDelivery)
{
    auto other = std::make_unique<ProjectController>(area.path() + "/other");
    ASSERT_EQ(other->openProject({target}).outcome, CommandOutcome::Applied);
    const auto context = OperationRunnerTestAccess::context(OperationTestAccess::runner(*other));
    ASSERT_TRUE(waitContext(context, true)); other.reset();
    EXPECT_TRUE(QFileInfo::exists(target)); EXPECT_TRUE(QFileInfo::exists(cachePath));
    EXPECT_EQ(context->decision, PublicationDecision::Discard);
}

TEST_F(Open, OpenAfterPublicationUncertainUsesActualManifest)
{
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    SaveTestAccess::configure(controller, {}, {}, [](QSaveFile &file) -> bool {
        const bool committed = file.commit(); EXPECT_TRUE(committed); throw std::runtime_error("lost commit receipt");
    });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_TRUE(controller.unconfirmedSave()); EXPECT_TRUE(controller.isDirty());
    open(); EXPECT_FALSE(controller.unconfirmedSave()); EXPECT_FALSE(controller.isDirty());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
}

TEST_F(Open, RecheckFailureCancellationAndDirtyUndoPreserveSession)
{
    open(); ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    const auto stamp = controller.project()->operationStamp(); const auto snapshot = controller.savedProject()->snapshot.snapshotId;
    recheck(CacheAccess::AvailableLimited); EXPECT_TRUE(controller.isDirty());
    EXPECT_EQ(controller.project()->operationStamp(), stamp); EXPECT_EQ(controller.savedProject()->snapshot.snapshotId, snapshot);
    ASSERT_EQ(controller.undo().outcome, CommandOutcome::Applied); EXPECT_FALSE(controller.isDirty()); EXPECT_TRUE(controller.canRedo());
    ASSERT_EQ(controller.recheckPointCache(limits).outcome, CommandOutcome::Applied); ready();
    EXPECT_EQ(controller.cancelOperation().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.project()->operationStamp().projectIdentity, stamp.projectIdentity);
    EXPECT_TRUE(controller.canRedo()); EXPECT_FALSE(controller.isDirty());
    EXPECT_EQ(controller.recheckPointCache({1, 0, 2}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(controller.pointCacheAvailability()->access, CacheAccess::AvailableLimited);
}

TEST_F(Open, PartialProjectAllowsReplacementImportOpenAndClose)
{
    const auto cacheBytes = bytes(cachePath); ASSERT_TRUE(QFile::remove(cachePath)); partial(CacheAccess::Missing);
    EXPECT_EQ(controller.importAscXyz({source, limits}).failure->detail, ErrorDetail::ReplacementNotConfirmed);
    ASSERT_EQ(controller.importAscXyz({source, limits, true}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Succeeded);
    EXPECT_NE(controller.project()->projectId(), original.projectId);
    open(); EXPECT_EQ(controller.pointCacheAvailability()->access, CacheAccess::Missing);
    write(cachePath, cacheBytes); recheck(CacheAccess::AvailableLimited);
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
}

TEST_F(Open, RestoredWrongCacheIdIsNotAcceptedOnRecheck)
{
    auto data = bytes(cachePath); ASSERT_TRUE(QFile::remove(cachePath)); partial(CacheAccess::Missing);
    const auto stamp = controller.project()->operationStamp();
    // A different valid cache from a second import has the same geometry but another CacheId.
    ProjectController other(area.path() + "/different");
    ASSERT_EQ(other.importAscXyz({source, limits}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(other));
    write(cachePath, bytes(QString::fromStdWString(other.project()->dataset()->cachePath().wstring())));
    recheck(CacheAccess::Corrupt); EXPECT_EQ(controller.project()->operationStamp(), stamp);
    EXPECT_EQ(controller.project()->dataset()->cacheId, original.pointCacheId); EXPECT_FALSE(controller.isDirty());
    write(cachePath, data); recheck(CacheAccess::AvailableLimited);
}

TEST_F(Open, DomainRestoreRejectsInvalidCellAndDoesNotGenerateRevision)
{
    const auto data = *controller.project()->dataset(); const auto id = original.projectId;
    ProjectState restored(123, id, data, std::numeric_limits<double>::denorm_min());
    EXPECT_EQ(restored.inputRevision(), 0); EXPECT_EQ(restored.cell(), std::numeric_limits<double>::denorm_min());
    EXPECT_THROW(ProjectState(1, id, data, 0), std::invalid_argument);
    EXPECT_THROW(ProjectState(1, id, data, std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST_F(Open, OpenAfterUncertainUnpublishedCommitRestoresActualOldSnapshot)
{
    const auto before = bytes(target);
    ASSERT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    SaveTestAccess::configure(controller, {}, {}, [](QSaveFile &) { return false; });
    ASSERT_EQ(controller.saveProject().outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_TRUE(controller.unconfirmedSave()); EXPECT_EQ(bytes(target), before);
    open(); EXPECT_FALSE(controller.unconfirmedSave()); EXPECT_FALSE(controller.isDirty());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), original.cell);
}

TEST_F(Open, NonZeroZSourceMetadataIsRestoredWithoutSource)
{
    write(source, "-0 2 7\n3 -4 0\n");
    ASSERT_EQ(OperationTestAccess::import(controller, {source, limits, true},
        [](io::NonZeroZ) { return io::ZDecision::Continue; }).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(controller)); ASSERT_TRUE(controller.project()->dataset()->hasNonZeroZ);
    target = area.path() + "/nonzero/p.pc2dproj";
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    ASSERT_TRUE(QFile::remove(source)); open(); EXPECT_TRUE(controller.project()->dataset()->hasNonZeroZ);
    EXPECT_EQ(controller.pointCacheAvailability()->access, CacheAccess::AvailableLimited);
}

TEST_F(Open, MultiPageOpenAndRecheckHaveConstantBoundedReadsAndNoNewFiles)
{
    QByteArray text;
    for (int i = 0; i < 10000; ++i) text += QByteArray::number(i) + " 0 0\n";
    write(source, text);
    ASSERT_EQ(controller.importAscXyz({source, limits, true}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    target = area.path() + "/multipage/p.pc2dproj";
    ASSERT_EQ(controller.saveProject({target}).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    ASSERT_EQ(controller.operation().status, OperationStatus::Succeeded);
    cachePath = QString::fromStdWString(controller.project()->dataset()->cachePath().wstring());
    const auto before = bytes(cachePath);
    auto files = [&] {
        QStringList result;
        QDirIterator it(area.path(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) result.push_back(it.next()); result.sort(); return result;
    };
    const auto paths = files(); open();
    for (int i = 0; i < 2; ++i) {
        const auto &reads = controller.pointCacheAvailability()->reads;
        EXPECT_EQ(reads.pointPages, 0); EXPECT_EQ(reads.indexPages, 1); EXPECT_EQ(reads.readOperations, 3);
        EXPECT_EQ(reads.readBytes, 73728); EXPECT_LT(reads.bufferBytes, 1ULL << 20);
        EXPECT_EQ(files(), paths); EXPECT_EQ(bytes(cachePath), before);
        if (!i) recheck(CacheAccess::AvailableLimited);
    }
}

TEST_F(Open, ExhaustedRuntimeIdentityRejectsCandidateWithoutReplacingSession)
{
    const auto state = controller.project();
    ProjectControllerTestAccess::setLastIdentity(controller, std::numeric_limits<std::uint64_t>::max());
    OpenRequest request{target}; request.replacementConfirmed = true;
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_EQ(controller.operation().status, OperationStatus::Failed); EXPECT_EQ(controller.project(), state);
    EXPECT_FALSE(controller.isDirty()); EXPECT_TRUE(controller.canUndo());
}

TEST_F(Open, RecheckAdmissionAndOpenNoProject)
{
    ProjectController empty(area.path() + "/empty");
    EXPECT_EQ(empty.recheckPointCache().failure->category, CommandError::NoProject);
    EXPECT_EQ(empty.openProject({"relative.pc2dproj"}).failure->category, CommandError::InvalidValue);
    EXPECT_EQ(empty.openProject({target}).outcome, CommandOutcome::Applied);
    EXPECT_EQ(empty.recheckPointCache().failure->category, CommandError::Busy);
    ASSERT_TRUE(finish(empty)); EXPECT_FALSE(empty.isDirty());
    EXPECT_EQ(empty.recheckPointCache({0, 0, 2}).failure->category, CommandError::InvalidValue);
    EXPECT_EQ(empty.recheckPointCache({1ULL << 20, 0, 1}).outcome, CommandOutcome::Applied);
    ASSERT_TRUE(finish(empty)); EXPECT_EQ(empty.operation().status, OperationStatus::Failed);
    EXPECT_EQ(empty.requestShutdown().outcome, CommandOutcome::Applied);
    EXPECT_EQ(empty.recheckPointCache().failure->detail, ErrorDetail::ShuttingDown);
}

TEST_F(Open, ShutdownFromAcceptedOpenNotificationWaitsForTerminalRelease)
{
    bool seen = false;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        seen = true;
        EXPECT_EQ(controller.operation().decision, PublicationDecision::Accept);
        EXPECT_TRUE(controller.operation().active);
        EXPECT_EQ(controller.requestShutdown().outcome, CommandOutcome::Applied);
        EXPECT_EQ(controller.shutdownState(), ShutdownState::Closing);
    });
    OpenRequest request{target}; request.replacementConfirmed = true;
    ASSERT_EQ(controller.openProject(request).outcome, CommandOutcome::Applied); ASSERT_TRUE(finish(controller));
    EXPECT_TRUE(seen); EXPECT_EQ(controller.shutdownState(), ShutdownState::Ready);
    EXPECT_FALSE(controller.operation().active); EXPECT_FALSE(controller.isDirty());
    EXPECT_EQ(controller.project()->projectId(), original.projectId);
}
} // namespace
