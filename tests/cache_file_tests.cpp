#include "cache/cache_file.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>

using namespace cadcontour::cache;

namespace cadcontour::cache {
class CacheFileTestAccess {
public:
    static void setFlush(StagedFile &file, Status (*flush)(QFileDevice &)) { file.flush_ = flush; }
    static void close(StagedFile &file) { file.file_->close(); }
};
} // namespace cadcontour::cache

namespace {
constexpr std::array<std::byte, 8> payload{std::byte{'P'}, std::byte{'C'}, std::byte{'2'}, std::byte{'D'},
    std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
int validatorCalls = 0;
Status validateSmallFile(QIODevice &device, std::uint64_t size)
{
    ++validatorCalls;
    if (size != payload.size())
        return makeError(ErrorCode::CorruptCache, Phase::Publication, Operation::Decode, QStringLiteral("Invalid test size"));
    std::array<std::byte, 8> b{};
    if (auto s = file::readExact(device, b); !s) return s;
    if (b != payload)
        return makeError(ErrorCode::CorruptCache, Phase::Publication, Operation::Decode, QStringLiteral("Invalid test signature"));
    return success();
}
Status failFlush(QFileDevice &)
{
    return makeError(ErrorCode::FlushFailed, Phase::FileIO, Operation::Flush,
                     QStringLiteral("Injected flush failure"), 8, 3);
}
QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// Only an exact-transfer seam, not a filesystem mock. Unbuffered mode exposes
// short operations and a deterministic zero/negative return to production loops.
class ShortDevice : public QIODevice {
public:
    ShortDevice() { open(ReadWrite | Unbuffered); }
    QByteArray data;
    qint64 cursor = 0;
    qint64 chunk = 3;
    bool readError = false;
    bool writeError = false;
    bool zeroRead = false;
    bool zeroWrite = false;
    bool seekError = false;
    bool reportNotAtEnd = false;
    std::optional<qint64> failAfter;
    int readCalls = 0;
    int writeCalls = 0;
    std::optional<qint64> reportedPosition;
    qint64 pos() const override { return reportedPosition.value_or(cursor); }
    qint64 size() const override { return data.size(); }
    bool atEnd() const override { return !reportNotAtEnd && cursor >= data.size(); }
    bool seek(qint64 offset) override
    {
        if (seekError || offset < 0) { setErrorString(QStringLiteral("Injected seek failure")); return false; }
        if (!QIODevice::seek(offset)) return false;
        cursor = offset; return true;
    }
protected:
    qint64 readData(char *out, qint64 size) override
    {
        if (size == 0) return 0;
        ++readCalls;
        if (readError || (failAfter && cursor >= *failAfter)) {
            setErrorString(QStringLiteral("Injected read failure")); return -1;
        }
        if (zeroRead) return 0;
        const auto n = std::max<qint64>(0, std::min({size, chunk, data.size() - cursor}));
        std::memcpy(out, data.constData() + cursor, static_cast<std::size_t>(n)); cursor += n;
        return n;
    }
    qint64 writeData(const char *in, qint64 size) override
    {
        ++writeCalls;
        if (writeError || (failAfter && cursor >= *failAfter)) {
            setErrorString(QStringLiteral("Injected write failure")); return -1;
        }
        if (zeroWrite) return 0;
        const auto n = std::min(size, chunk);
        data.append(in, n); cursor += n; return n;
    }
};
} // namespace

TEST(CacheFileIO, ExactTransfersHandleShortOperations)
{
    ShortDevice d;
    ASSERT_TRUE(file::writeExact(d, payload));
    EXPECT_GE(d.writeCalls, 3);
    EXPECT_EQ(d.data, QByteArray("PC2Dtest"));
    ASSERT_TRUE(file::seek(d, 0));
    std::array<std::byte, 8> b{};
    ASSERT_TRUE(file::readExact(d, b));
    EXPECT_EQ(b, payload); EXPECT_GE(d.readCalls, 3);
    EXPECT_TRUE(file::writeExact(d, {})); EXPECT_TRUE(file::readExact(d, {}));
}

TEST(CacheFileIO, EofReadErrorAndZeroProgressAreDistinct)
{
    ShortDevice d; d.data = "12345";
    std::array<std::byte, 8> b{};
    auto result = file::readExact(d, b);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::IncompleteCache);
    EXPECT_EQ(result.error().offset, 5);
    ASSERT_TRUE(file::seek(d, 0)); d.readError = true;
    result = file::readExact(d, b);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ReadFailed);
    EXPECT_EQ(result.error().detail, QStringLiteral("Injected read failure"));
    d.readError = false; d.zeroRead = true; d.reportNotAtEnd = true;
    const auto calls = d.readCalls;
    result = file::readExact(d, b);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ReadFailed);
    EXPECT_LE(d.readCalls - calls, 2);
}

TEST(CacheFileIO, WriteErrorsZeroProgressSeekAndOffsetLimits)
{
    ShortDevice d; d.zeroWrite = true;
    EXPECT_EQ(file::writeExact(d, payload).error().code, ErrorCode::WriteFailed);
    EXPECT_EQ(d.writeCalls, 1);
    d.zeroWrite = false; d.writeError = true;
    EXPECT_EQ(file::writeExact(d, payload).error().detail, QStringLiteral("Injected write failure"));
    d.seekError = true;
    EXPECT_EQ(file::seek(d, 4).error().code, ErrorCode::SeekFailed);
    EXPECT_EQ(file::seek(d, UINT64_MAX).error().code, ErrorCode::InvalidInput);
    d.reportedPosition = INT64_MAX;
    EXPECT_EQ(file::writeExact(d, payload).error().code, ErrorCode::InvalidInput);
    std::array<std::byte, 8> b{};
    EXPECT_EQ(file::readExact(d, b).error().code, ErrorCode::InvalidInput);
    d.close(); EXPECT_FALSE(file::readExact(d, {})); EXPECT_FALSE(file::writeExact(d, {}));
}

TEST(CacheFileIO, FailureAfterPartialTransferReportsTheFailingOffset)
{
    ShortDevice d; d.failAfter = 3;
    auto written = file::writeExact(d, payload);
    ASSERT_FALSE(written); EXPECT_EQ(written.error().code, ErrorCode::WriteFailed);
    EXPECT_EQ(written.error().offset, 3); EXPECT_EQ(d.data, QByteArray("PC2"));
    d.data = "PC2Dtest"; ASSERT_TRUE(file::seek(d, 0));
    std::array<std::byte, 8> b{};
    auto read = file::readExact(d, b);
    ASSERT_FALSE(read); EXPECT_EQ(read.error().code, ErrorCode::ReadFailed);
    EXPECT_EQ(read.error().offset, 3);
}

TEST(CacheFileIO, RealFileSizeFlushAndCleanup)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    QString temporary;
    {
        auto made = StagedFile::create(dir.filePath(QStringLiteral("final.bin"))); ASSERT_TRUE(made);
        auto owner = std::move(made.value()); temporary = owner->temporaryPath();
        EXPECT_TRUE(QFile::exists(temporary)); EXPECT_EQ(QFileInfo(temporary).absolutePath(), dir.path());
        ASSERT_TRUE(owner->write(payload)); ASSERT_TRUE(owner->flush());
        ASSERT_TRUE(owner->size()); EXPECT_EQ(owner->size().value(), 8);
        ASSERT_TRUE(owner->seek(0)); std::array<std::byte, 8> b{};
        ASSERT_TRUE(owner->read(b)); EXPECT_EQ(b, payload);
    }
    EXPECT_FALSE(QFile::exists(temporary));
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
    QFile closed(dir.filePath(QStringLiteral("closed.bin")));
    EXPECT_EQ(file::flush(closed).error().code, ErrorCode::FlushFailed);
    EXPECT_FALSE(file::size(closed));
}

TEST(CachePublication, SuccessfulRenameGuardAndExplicitOwnershipTransfer)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("final.bin"));
    QString temporary;
    {
        auto made = StagedFile::create(path); ASSERT_TRUE(made);
        auto owner = std::move(made.value()); temporary = owner->temporaryPath();
        ASSERT_TRUE(owner->write(payload));
        auto result = owner->publish(8, validateSmallFile); ASSERT_TRUE(result);
        owner.reset(); // destruction of old owner must not delete renamed guard's file
        EXPECT_FALSE(QFile::exists(temporary)); EXPECT_TRUE(QFile::exists(path));
        EXPECT_EQ(result.value().path(), path);
        EXPECT_EQ(readFile(path), QByteArray("PC2Dtest"));
        EXPECT_EQ(result.value().releaseToOwner(), path);
        EXPECT_TRUE(result.value().path().isEmpty());
    }
    EXPECT_TRUE(QFile::exists(path)); EXPECT_EQ(readFile(path), QByteArray("PC2Dtest"));
}

TEST(CachePublication, UnacceptedPublishedGuardCleansUpAndMoveTransfersOwnership)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("final.bin"));
    {
        auto made = StagedFile::create(path); ASSERT_TRUE(made);
        ASSERT_TRUE(made.value()->write(payload));
        auto published = made.value()->publish(8, validateSmallFile); ASSERT_TRUE(published);
        PublishedFile moved(std::move(published.value()));
        EXPECT_TRUE(QFile::exists(path)); EXPECT_EQ(moved.path(), path);
    }
    EXPECT_FALSE(QFile::exists(path));
}

TEST(CachePublication, ExistingTargetIsNeverReplacedOrRemoved)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("existing.bin"));
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); ASSERT_EQ(f.write("foreign"), 7); }
    QString temporary;
    {
        auto made = StagedFile::create(path); ASSERT_TRUE(made);
        temporary = made.value()->temporaryPath(); ASSERT_TRUE(made.value()->write(payload));
        const auto published = made.value()->publish(8, validateSmallFile);
        ASSERT_FALSE(published); EXPECT_EQ(published.error().code, ErrorCode::PublishFailed);
        EXPECT_EQ(published.error().phase, Phase::Publication);
        EXPECT_NE(published.error().deviceCode, 0);
        EXPECT_TRUE(QFile::exists(temporary));
        EXPECT_EQ(readFile(path), QByteArray("foreign"));
        EXPECT_FALSE(made.value()->publish(8, validateSmallFile)); // one-shot, no retry
    }
    EXPECT_FALSE(QFile::exists(temporary)); EXPECT_EQ(readFile(path), QByteArray("foreign"));
}

TEST(CachePublication, RenameFailureDoesNotRemoveForeignDirectory)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("foreign directory"));
    ASSERT_TRUE(QDir().mkdir(path));
    QString temporary;
    {
        auto made = StagedFile::create(path); ASSERT_TRUE(made);
        temporary = made.value()->temporaryPath(); ASSERT_TRUE(made.value()->write(payload));
        EXPECT_EQ(made.value()->publish(8, validateSmallFile).error().code, ErrorCode::PublishFailed);
    }
    EXPECT_FALSE(QFile::exists(temporary)); EXPECT_TRUE(QFileInfo(path).isDir());
}

TEST(CachePublication, NativeRenameFailureToInvalidWindowsNameIsTerminal)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("invalid?.bin"));
    QString temporary;
    {
        auto made = StagedFile::create(path); ASSERT_TRUE(made);
        temporary = made.value()->temporaryPath(); ASSERT_TRUE(made.value()->write(payload));
        const auto result = made.value()->publish(8, validateSmallFile);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::PublishFailed);
        EXPECT_FALSE(QFile::exists(path)); EXPECT_TRUE(QFile::exists(temporary));
        EXPECT_FALSE(made.value()->publish(8, validateSmallFile));
    }
    EXPECT_FALSE(QFile::exists(temporary));
}

TEST(CachePublication, FlushFailureBlocksValidationAndRenameAndPreservesFirstError)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("final.bin"));
    QString temporary; validatorCalls = 0;
    {
        auto made = StagedFile::create(path); ASSERT_TRUE(made);
        temporary = made.value()->temporaryPath(); ASSERT_TRUE(made.value()->write(payload));
        CacheFileTestAccess::setFlush(*made.value(), failFlush);
        const auto published = made.value()->publish(8, validateSmallFile);
        ASSERT_FALSE(published); EXPECT_EQ(published.error().code, ErrorCode::FlushFailed);
        EXPECT_EQ(published.error().deviceCode, 3); EXPECT_EQ(published.error().offset, 8);
        EXPECT_EQ(validatorCalls, 0); EXPECT_FALSE(QFile::exists(path));
        const auto later = made.value()->seek(UINT64_MAX);
        EXPECT_EQ(later.error().code, ErrorCode::FlushFailed);
        EXPECT_EQ(later.error().detail, published.error().detail);
    }
    EXPECT_FALSE(QFile::exists(temporary)); EXPECT_FALSE(QFile::exists(path));
}

TEST(CachePublication, EarlierSeekOrWriteFailureCannotBecomeSuccess)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    for (bool writeFailure : {false, true}) {
        const auto path = dir.filePath(writeFailure ? QStringLiteral("write.bin") : QStringLiteral("seek.bin"));
        auto made = StagedFile::create(path); ASSERT_TRUE(made);
        ASSERT_TRUE(made.value()->write(payload));
        if (writeFailure) {
            CacheFileTestAccess::close(*made.value()); EXPECT_FALSE(made.value()->write(payload));
        } else {
            EXPECT_FALSE(made.value()->seek(UINT64_MAX));
        }
        const auto first = *made.value()->firstError();
        EXPECT_EQ(made.value()->flush().error().operation, first.operation);
        EXPECT_EQ(made.value()->size().error().operation, first.operation);
        EXPECT_EQ(made.value()->publish(8, validateSmallFile).error().operation, first.operation);
        EXPECT_FALSE(QFile::exists(path));
    }
}

TEST(CachePublication, SizeAndStructureValidationAreRequiredAndTerminal)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    for (int scenario = 0; scenario < 4; ++scenario) {
        const auto path = dir.filePath(QString::number(scenario));
        QString temporary;
        {
            auto made = StagedFile::create(path); ASSERT_TRUE(made); temporary = made.value()->temporaryPath();
            auto bytes = payload; if (scenario == 1) bytes[0] = std::byte{0};
            ASSERT_TRUE(made.value()->write(bytes));
            const auto result = made.value()->publish(scenario == 0 ? 9 : scenario == 3 ? UINT64_MAX : 8,
                                                       scenario == 2 ? nullptr : validateSmallFile);
            EXPECT_FALSE(result); EXPECT_FALSE(QFile::exists(path));
            EXPECT_FALSE(made.value()->write(payload));
            EXPECT_FALSE(made.value()->publish(8, validateSmallFile));
        }
        EXPECT_FALSE(QFile::exists(temporary));
    }
}

TEST(CachePublication, UnicodePathsSpacesAndUniqueTemporaryNames)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto folder = dir.filePath(QString::fromUtf8("Каталог с пробелами 中文"));
    ASSERT_TRUE(QDir().mkdir(folder));
    const auto path = QDir(folder).filePath(QString::fromUtf8("точки 源.bin"));
    auto first = StagedFile::create(path); auto second = StagedFile::create(path);
    ASSERT_TRUE(first); ASSERT_TRUE(second);
    EXPECT_NE(first.value()->temporaryPath(), second.value()->temporaryPath());
    ASSERT_TRUE(first.value()->write(payload));
    auto published = first.value()->publish(8, validateSmallFile); ASSERT_TRUE(published);
    EXPECT_EQ(published.value().releaseToOwner(), path); EXPECT_TRUE(QFile::exists(path));
    const auto otherTemporary = second.value()->temporaryPath(); second.value().reset();
    EXPECT_FALSE(QFile::exists(otherTemporary)); EXPECT_EQ(readFile(path), QByteArray("PC2Dtest"));
}

TEST(CachePublication, InvalidDestinationDoesNotCreateFiles)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(StagedFile::create({}));
    EXPECT_FALSE(StagedFile::create(dir.filePath(QStringLiteral("missing/points.bin"))));
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
}
