#include "io/asc_xyz_importer.h"
#include "cache/cache_reader.h"
#include "fixtures/import/binary64_golden.h"
#include "point_stream_sort_test_access.h"

#include <QDir>
#include <QBuffer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <bit>
#include <new>
#include <type_traits>
#include <vector>

using namespace cadcontour::cache;
using namespace cadcontour::io;

namespace cadcontour::cache {
// This executable needs only the existing private Writer/Sorter fault seams.
class CacheWriterTestAccess {
public:
    static auto &sorter(CacheWriter &writer) { return *writer.sorter_; }
    static void hook(CacheWriter &writer, Status (*fn)(CacheWriter &, Operation)) { writer.ioHook_ = fn; }
};
}

namespace {
static_assert(std::is_nothrow_copy_constructible_v<Error>);
static_assert(std::is_nothrow_move_constructible_v<Error>);
static_assert(std::is_nothrow_constructible_v<std::variant<SourceSummary, Error>, Error &&>);
static_assert(std::is_nothrow_constructible_v<std::variant<ImportProduct, Error>, Error &&>);
ResourceLimits limits() { return {8ULL << 20, 64ULL << 20, 6}; }
void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); ASSERT_EQ(file.write(bytes), bytes.size());
}
QStringList children(const QTemporaryDir &dir)
{
    return QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
}
auto continueZ = [](NonZeroZ) { return ZDecision::Continue; };
}

TEST(AscImport, GoldenRoundTripByRecordIdMetadataAndDiscard)
{
    for (const auto *extension : {"asc", "xyz"}) {
        QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
        const auto source = dir.filePath(QString::fromUtf8("облако 测量.") + extension);
        const auto target = dir.filePath("result.pc2dcache");
        QByteArray text;
        for (const auto &g : asc_test::golden) text += QByteArray(g.token) + " " + g.token + " -0\r\n";
        text += "-0 +0 1\n-0 +0 0\n"; write(source, text);
        {
            auto result = importAscXyz(source, target, dir.path(), limits(), continueZ);
            ASSERT_TRUE(result) << result.error().detail.toStdString();
            auto &product = result.value();
            ASSERT_TRUE(QFileInfo::exists(target)); EXPECT_EQ(product.source.bytesRead, text.size());
            EXPECT_EQ(product.source.firstNonZeroZ->line, asc_test::golden.size() + 1);
            const auto &meta = product.cache.metadata();
            EXPECT_EQ(meta.pointCount, asc_test::golden.size() + 2); EXPECT_TRUE(meta.hasNonZeroZ);
            EXPECT_EQ(std::bit_cast<std::uint64_t>(meta.bounds.maxX), 0x7fefffffffffffffULL);
            EXPECT_EQ(std::bit_cast<std::uint64_t>(meta.bounds.minY), 0xffefffffffffffffULL);
            auto opened = CacheReader::open(target, limits()); ASSERT_TRUE(opened);
            auto &reader = *opened.value();
            EXPECT_EQ(reader.metadata().id, meta.id); EXPECT_EQ(reader.metadata().pointCount, meta.pointCount);
            EXPECT_TRUE(reader.metadata().hasNonZeroZ);
            std::vector<bool> seen(static_cast<std::size_t>(meta.pointCount));
            bool sortedAwayFromSource = false; RecordId ordinal = 0;
            auto scan = reader.scanAll([&](auto batch) {
                for (const auto &record : batch) {
                    EXPECT_LT(record.id, seen.size()); if (record.id >= seen.size()) continue;
                    const auto id = static_cast<std::size_t>(record.id);
                    EXPECT_FALSE(seen[id]); seen[id] = true;
                    sortedAwayFromSource |= ordinal++ != record.id;
                    const auto x = id < asc_test::golden.size() ? asc_test::golden[id].bits : 0x8000000000000000ULL;
                    const auto y = id < asc_test::golden.size() ? asc_test::golden[id].bits : 0ULL;
                    EXPECT_EQ(std::bit_cast<std::uint64_t>(record.point.x), x);
                    EXPECT_EQ(std::bit_cast<std::uint64_t>(record.point.y), y);
                }
                return success();
            });
            ASSERT_TRUE(scan); EXPECT_TRUE(sortedAwayFromSource);
            for (bool found : seen) EXPECT_TRUE(found);
            auto validation = reader.validate(limits(), dir.path()); ASSERT_TRUE(validation);
            EXPECT_EQ(children(dir).size(), 2);
        }
        EXPECT_FALSE(QFileInfo::exists(target)); EXPECT_EQ(children(dir).size(), 1);
    }
}

TEST(AscImport, EmptyMalformedZCancelAndOpenFailureCleanUp)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto source = dir.filePath("source.xyz"), target = dir.filePath("output.cache");
    auto missing = importAscXyz(source, target, dir.path(), limits(), continueZ);
    ASSERT_FALSE(missing); EXPECT_EQ(missing.error().code, ErrorCode::OpenFailed);
    EXPECT_NE(missing.error().deviceCode, 0);
    for (const auto &input : {QByteArray{}, QByteArray("1 2 0\n3 4 0\nx y z"), QByteArray("1 2 1")}) {
        write(source, input);
        auto result = importAscXyz(source, target, dir.path(), limits(), [](auto) { return ZDecision::Cancel; });
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, input.isEmpty() ? ErrorCode::InsufficientData
            : input.contains('x') ? ErrorCode::ParseError : ErrorCode::Cancelled);
        EXPECT_EQ(children(dir).size(), 1);
    }
}

TEST(AscImport, DetectableSourceGrowthAndTruncation)
{
    for (bool truncate : {false, true}) {
        QTemporaryDir dir; const auto source = dir.filePath("source.asc");
        write(source, "1 2 1\n3 4 0\n");
        auto result = importAscXyz(source, dir.filePath("output.cache"), dir.path(), limits(), [&](auto) {
            QFile changing(source); EXPECT_TRUE(changing.open(QIODevice::ReadWrite));
            if (truncate) EXPECT_TRUE(changing.resize(6));
            else { EXPECT_TRUE(changing.seek(changing.size())); EXPECT_EQ(changing.write("5 6 0\n"), 6); }
            EXPECT_TRUE(changing.flush()); return ZDecision::Continue;
        });
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::SourceChanged);
        EXPECT_EQ(children(dir).size(), 1);
    }
}

TEST(AscImport, BudgetsIncludeParserAndSourceHandle)
{
    QTemporaryDir dir; const auto source = dir.filePath("source.xyz"); write(source, "1 2 0");
    for (const auto budget : {ResourceLimits{ascParserReserve, 1ULL << 20, 6}, ResourceLimits{8ULL << 20, 1ULL << 20, 4},
                              ResourceLimits{8ULL << 20, 0, 6}}) {
        auto result = importAscXyz(source, dir.filePath("output.cache"), dir.path(), budget, continueZ);
        ASSERT_FALSE(result); EXPECT_TRUE(result.error().code == ErrorCode::ResourceExhausted || result.error().code == ErrorCode::InvalidInput);
        EXPECT_EQ(children(dir).size(), 1);
    }
}

TEST(AscImport, AppendSpillAndFinishFailuresPreserveCauseAndExistingFiles)
{
    QTemporaryDir dir; const auto source = dir.filePath("source.xyz"), target = dir.filePath("existing.cache");
    write(target, "accepted cache sentinel");
    // A tiny sort disk partition fails while append spills its first bounded run.
    write(source, QByteArray("1 2 0\n").repeated(240000));
    auto low = limits(); low.temporaryDiskBytes = 16384;
    auto append = importAscXyz(source, dir.filePath("other.cache"), dir.path(), low, continueZ);
    ASSERT_FALSE(append); EXPECT_EQ(append.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(append.error().phase, Phase::RunWrite);
    write(source, "1 2 0");
    auto finish = importAscXyz(source, target, dir.path(), limits(), continueZ);
    ASSERT_FALSE(finish); EXPECT_EQ(finish.error().code, ErrorCode::PublishFailed);
    QFile accepted(target); ASSERT_TRUE(accepted.open(QIODevice::ReadOnly)); EXPECT_EQ(accepted.readAll(), "accepted cache sentinel");
    EXPECT_EQ(children(dir).size(), 2);
}

TEST(AscImport, EveryCancellationCheckpointIncludingBeforeFinishAndWriter)
{
    struct Cancel { int calls = 0, stop = 0; } state;
    auto check = [](void *p) noexcept { auto &s = *static_cast<Cancel *>(p); return ++s.calls == s.stop; };
    QTemporaryDir dir; const auto source = dir.filePath("source.xyz"), target = dir.filePath("output.cache");
    write(source, "1 2 0\n3 4 0");
    { auto baseline = importAscXyz(source, target, dir.path(), limits(), continueZ, check, &state); ASSERT_TRUE(baseline); }
    const auto total = state.calls; EXPECT_GT(total, 10);
    for (int stop = 1; stop <= total; ++stop) {
        state = {0, stop}; SCOPED_TRACE(stop);
        auto result = importAscXyz(source, target, dir.path(), limits(), continueZ, check, &state);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
        EXPECT_EQ(children(dir).size(), 1);
    }
}

TEST(AscImport, InjectedAppendAndFinishIoFailuresThroughParserPipeline)
{
    for (bool failAppend : {true, false}) {
        QTemporaryDir dir; const auto target = dir.filePath("output.cache");
        {
            auto created = CacheWriter::create(target, limits(), dir.path()); ASSERT_TRUE(created);
            auto writer = std::move(created.value());
            auto &sorter = CacheWriterTestAccess::sorter(*writer);
            const auto capacity = PointStreamSortTestAccess::capacity(sorter);
            if (failAppend) {
                PointStreamSortTestAccess::hook(sorter, [](PointStreamSort &, Operation op, QFile &) -> Status {
                    return op == Operation::Write ? Status{makeError(ErrorCode::WriteFailed, Phase::RunWrite,
                        Operation::Write, "injected append write", 123, 7)} : success();
                });
            } else {
                CacheWriterTestAccess::hook(*writer, [](CacheWriter &, Operation op) -> Status {
                    return op == Operation::Write ? Status{makeError(ErrorCode::WriteFailed, Phase::PointPages,
                        Operation::Write, "injected finish write", 456, 7)} : success();
                });
            }
            QBuffer source;
            source.setData(QByteArray("1 2 0\n").repeated(static_cast<qsizetype>(failAppend ? capacity + 1 : 1)));
            ASSERT_TRUE(source.open(QIODevice::ReadOnly));
            auto parsed = parseAscXyz(source, [&](auto batch) { return writer->append(batch); }, {});
            if (failAppend) {
                ASSERT_FALSE(parsed); EXPECT_EQ(parsed.error().code, ErrorCode::WriteFailed);
                EXPECT_EQ(parsed.error().detail, "injected append write"); EXPECT_EQ(parsed.error().offset, 123);
                EXPECT_EQ(parsed.error().deviceCode, 7);
            } else {
                ASSERT_TRUE(parsed); auto finished = writer->finish(); ASSERT_FALSE(finished);
                EXPECT_EQ(finished.error().code, ErrorCode::WriteFailed);
                EXPECT_EQ(finished.error().detail, "injected finish write"); EXPECT_EQ(finished.error().offset, 456);
            }
        }
        EXPECT_TRUE(children(dir).isEmpty());
    }
}

TEST(AscImportReviewRegression, BadAllocPropagatesThroughImportResultAndCleansWriter)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto source = dir.filePath("source.xyz"), target = dir.filePath("output.cache");
    write(source, "1 2 1\n");
    auto result = importAscXyz(source, target, dir.path(), limits(), [](auto) -> ZDecision {
        throw std::bad_alloc{};
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(result.error().phase, Phase::Parse); // original parser error is preserved
    EXPECT_EQ(result.error().detail, QStringLiteral("ASC/XYZ allocation failed"));
    EXPECT_EQ(result.error().detail.capacity(), 0);
    EXPECT_FALSE(result.error().detail.isDetached());
    EXPECT_FALSE(result.error().offset); EXPECT_FALSE(result.error().source);
    EXPECT_EQ(children(dir), QStringList{"source.xyz"});
}
