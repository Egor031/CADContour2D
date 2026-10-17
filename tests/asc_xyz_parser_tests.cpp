#include "io/asc_xyz_parser.h"
#include "cache/cache_writer.h"
#include "fixtures/import/binary64_golden.h"

#include <QBuffer>
#include <QLocale>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <vector>

using namespace cadcontour::cache;
using namespace cadcontour::io;

namespace {
// No QIODevice read-ahead: each readData call is one controlled short read.
class Source : public QIODevice {
public:
    QByteArray bytes;
    qint64 block = 7, position = 0, failure = -1, zeroProgress = -1;
    int reads = 0;
    explicit Source(QByteArray input) : bytes(std::move(input)) { open(ReadOnly | Unbuffered); }
    bool isSequential() const override { return true; }
    bool atEnd() const override { return position == bytes.size(); }
protected:
    qint64 readData(char *out, qint64 maximum) override
    {
        ++reads;
        if (failure >= 0 && position >= failure) { setErrorString("injected source failure"); return -1; }
        if (zeroProgress >= 0 && position >= zeroProgress) return 0;
        auto n = std::min({maximum, block, bytes.size() - position});
        if (failure >= 0) n = std::min(n, failure - position);
        std::copy_n(bytes.constData() + position, n, out); position += n;
        return n;
    }
    qint64 writeData(const char *, qint64) override { return -1; }
};
struct Parsed {
    std::vector<Measurement> records;
    int requests = 0;
    Result<SourceSummary> result;
    explicit Parsed(QByteArray input, qint64 block = 7) : result(SourceSummary{})
    {
        Source source(std::move(input)); source.block = block;
        result = parseAscXyz(source, [&](auto batch) {
            EXPECT_LE(batch.size(), ascBatchSize);
            records.insert(records.end(), batch.begin(), batch.end()); return success();
        }, [&](NonZeroZ) { ++requests; return ZDecision::Continue; });
    }
};
std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }
}

TEST(AscParser, WhitespaceLineEndingsBomAndFinalRecord)
{
    const QByteArray input = QByteArray::fromHex("efbbbf") + "\n \t\v\f\r\n +1\t-.5\v-0\f \r\n1.  +1.5E+4  0\n.5 -1.25e-3 0";
    for (qint64 block : {1, 2, 3, 7, 65536}) {
        Parsed p(input, block); ASSERT_TRUE(p.result);
        EXPECT_EQ(p.result.value().bytesRead, input.size());
        EXPECT_EQ(p.result.value().physicalLines, 5); EXPECT_EQ(p.result.value().measurements, 3);
        ASSERT_EQ(p.records.size(), 3); EXPECT_EQ(p.requests, 0);
        EXPECT_EQ(bits(p.records[0].z), 0x8000000000000000ULL);
        EXPECT_EQ(p.records[0].x, 1.); EXPECT_EQ(p.records[0].y, -.5);
        EXPECT_EQ(p.records[1].y, 15000.);
    }
}

class BadRecord : public testing::TestWithParam<const char *> {};
TEST_P(BadRecord, RejectsWithoutSkipping)
{
    Parsed p(QByteArray("\n1 2 0\n") + GetParam());
    ASSERT_FALSE(p.result); EXPECT_EQ(p.result.error().code, ErrorCode::ParseError);
    ASSERT_TRUE(p.result.error().source); EXPECT_EQ(p.result.error().source->line, 3);
}
INSTANTIATE_TEST_SUITE_P(Format, BadRecord, testing::Values(
    "1", "1 2", "1 2 3 4", "# comment", "X Y Z", "1 2 0 # comment", "1,2 3 0",
    "NaN 1 0", "Inf 1 0", "-Infinity 1 0", "0x1p2 1 0", "1D2 1 0", "1f 2 0",
    "+ 1 0", "-. 1 0", ". 1 0", "1e 1 0", "1e+ 1 0", "--1 1 0", "1..2 1 0"));

TEST(AscParser, RejectsUnsupportedBytesAndCarriageReturns)
{
    const std::vector<QByteArray> invalid{
        QByteArray("1 2\0 0", 7), "1 2 0\r", "1 2 0\r1 2 0", QByteArray::fromHex("fffe3100"),
        QByteArray::fromHex("feff0031"), QByteArray::fromHex("efbb"), QByteArray::fromHex("ef00"),
        QByteArray("1 2 0\n") + QByteArray::fromHex("efbbbf") + "1 2 0", "1 2 0\x7f", "1 2 0\xc2\xa0"};
    for (const auto &input : invalid) {
        for (qint64 block : {1, 7}) {
            Parsed p(input, block); ASSERT_FALSE(p.result); EXPECT_EQ(p.result.error().code, ErrorCode::ParseError);
            EXPECT_TRUE(p.result.error().source);
        }
    }
}

TEST(AscParser, EmptyIsInsufficientData)
{
    for (const auto &input : {QByteArray{}, QByteArray("\n\r\n \t\v\f"), QByteArray::fromHex("efbbbf")}) {
        Parsed p(input); ASSERT_FALSE(p.result); EXPECT_EQ(p.result.error().code, ErrorCode::InsufficientData);
    }
}

TEST(AscParser, BoundariesAndExactLineLimit)
{
    QByteArray line(ascLineBytes - 5, ' '); line += "1 2 0";
    for (const auto &ending : {QByteArray{}, QByteArray("\n"), QByteArray("\r\n")}) {
        for (qint64 block : {1, 13, 65536}) {
            Parsed p(line + ending, block); ASSERT_TRUE(p.result); EXPECT_EQ(p.records.size(), 1);
        }
    }
    Parsed longLine(line + " \n"); ASSERT_FALSE(longLine.result);
    EXPECT_EQ(longLine.result.error().source->column, 65537);
    // CR and LF on separate read blocks, token across blocks, LF on block boundary.
    for (const auto &input : {QByteArray("1.25e-3 2 0\r\n3 4 0"), QByteArray("1 2 0\n3 4 0\n")}) {
        for (qint64 block : {2, 6, 12}) { Parsed p(input, block); ASSERT_TRUE(p.result); EXPECT_EQ(p.records.size(), 2); }
    }
    Parsed many(QByteArray("1 2 0\n").repeated(20000), 17);
    ASSERT_TRUE(many.result); EXPECT_EQ(many.records.size(), 20000); EXPECT_EQ(many.result.value().physicalLines, 20000);
}

TEST(AscParser, BoundedDiagnosticAndCoordinate)
{
    Parsed p("\n \n 1 " + QByteArray(60000, 'x') + " 0\n");
    ASSERT_FALSE(p.result); const auto &where = *p.result.error().source;
    EXPECT_EQ(where.line, 3); EXPECT_EQ(where.column, 4); EXPECT_EQ(where.coordinate, Coordinate::Y);
    EXPECT_EQ(where.token.size(), 64); EXPECT_LE(p.result.error().detail.size(), 256);
    Parsed missing("1 2"); EXPECT_EQ(missing.result.error().source->coordinate, Coordinate::Z);
}

TEST(AscParser, IndependentBinary64GoldenBitsForEveryCoordinate)
{
    struct LocaleGuard { QLocale previous; ~LocaleGuard() { QLocale::setDefault(previous); } } guard;
    QLocale::setDefault(QLocale(QLocale::Russian));
    for (const auto &golden : asc_test::golden) {
        SCOPED_TRACE(golden.token);
        const QByteArray token(golden.token);
        Parsed p(token + " " + token + " " + token + "\n", 11);
        ASSERT_TRUE(p.result); ASSERT_EQ(p.records.size(), 1);
        EXPECT_EQ(bits(p.records[0].x), golden.bits); EXPECT_EQ(bits(p.records[0].y), golden.bits);
        EXPECT_EQ(bits(p.records[0].z), golden.bits);
    }
}

TEST(AscParser, RejectsOverflowAndNonzeroUnderflow)
{
    for (const auto *token : asc_test::rangeErrors) {
        Parsed p(QByteArray("0 ") + token + " 0"); ASSERT_FALSE(p.result);
        EXPECT_EQ(p.result.error().code, ErrorCode::NumericRange);
    }
    for (const auto *token : {"1e309", "-1e99999999999999999999999999", "1e-324", "-1e-99999",
                              "1.7976931348623159e308", "2.4703282292062327e-324"}) {
        Parsed p(QByteArray("0 ") + token + " 0"); ASSERT_FALSE(p.result);
        EXPECT_EQ(p.result.error().code, ErrorCode::NumericRange);
        EXPECT_EQ(p.result.error().source->coordinate, Coordinate::Y);
    }
}

TEST(AscParser, SingleZDecisionAtFirstFullyValidatedRecord)
{
    for (int first : {0, 1, 2}) {
        Source source("1 2 0\n1 2 0\n1 2 0\n"); source.bytes[first * 6 + 4] = '1'; source.block = 6;
        int requests = 0, deliveries = 0;
        auto result = parseAscXyz(source, [&](auto) { ++deliveries; return success(); }, [&](NonZeroZ z) {
            ++requests; EXPECT_EQ(z.line, first + 1); EXPECT_EQ(deliveries, 0);
            EXPECT_EQ(source.position, (first + 1) * 6); return ZDecision::Continue;
        });
        ASSERT_TRUE(result); EXPECT_EQ(requests, 1); EXPECT_EQ(deliveries, 1);
    }
    Parsed repeated("1 2 4.9406564584124654e-324\n1 2 1\n"); ASSERT_TRUE(repeated.result); EXPECT_EQ(repeated.requests, 1);
    Parsed malformed("1 2 1 extra"); EXPECT_EQ(malformed.requests, 0);
    Source source("1 2 1\n3 4 0\n"); source.block = 6;
    auto cancelled = parseAscXyz(source, [&](auto) { ADD_FAILURE(); return success(); }, [](auto) { return ZDecision::Cancel; });
    ASSERT_FALSE(cancelled); EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled); EXPECT_EQ(source.position, 6);
    Source absent("1 2 1");
    auto noHandler = parseAscXyz(absent, [](auto) { return success(); }, {});
    EXPECT_EQ(noHandler.error().code, ErrorCode::InvalidInput);
}

TEST(AscParser, ReadFailureAndZeroProgressAreNotEof)
{
    for (bool noProgress : {false, true}) {
        Source source("1 2 0\n3 4 0\n"); source.block = 6;
        if (noProgress) source.zeroProgress = 6; else source.failure = 6;
        auto result = parseAscXyz(source, [](auto) { return success(); }, {});
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ReadFailed);
        EXPECT_EQ(result.error().phase, Phase::SourceRead); EXPECT_EQ(result.error().offset, 6);
        if (!noProgress) EXPECT_EQ(result.error().detail, "injected source failure");
    }
}

TEST(AscParser, SinkFailureExceptionsAndCancellation)
{
    Source source("1 2 0");
    auto failed = parseAscXyz(source, [](auto) -> Status {
        return makeError(ErrorCode::WriteFailed, Phase::RunWrite, Operation::Write, "injected append");
    }, {});
    ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code, ErrorCode::WriteFailed); EXPECT_EQ(failed.error().phase, Phase::RunWrite);
    Source throwing("1 2 1");
    auto exception = parseAscXyz(throwing, [](auto) { return success(); }, [](auto) -> ZDecision { throw std::runtime_error("handler"); });
    EXPECT_EQ(exception.error().code, ErrorCode::ConsumerFailed);
    struct Cancellation { int checks = 0; } state;
    Source longSource(QByteArray(65000, ' ')); longSource.block = 65536;
    auto result = parseAscXyz(longSource, [](auto) { return success(); }, {}, [](void *p) noexcept {
        return ++static_cast<Cancellation *>(p)->checks == 9;
    }, &state);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled); EXPECT_EQ(state.checks, 9);
}

TEST(AscParser, RejectsInvalidDeviceContracts)
{
    QBuffer closed; auto result = parseAscXyz(closed, [](auto) { return success(); }, {});
    EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
    QBuffer text; text.setData("1 2 0"); ASSERT_TRUE(text.open(QIODevice::ReadOnly | QIODevice::Text));
    result = parseAscXyz(text, [](auto) { return success(); }, {}); EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
    Source input("1 2 0"); result = parseAscXyz(input, {}, {}); EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
}

TEST(AscParser, ReadFailureAfterDeliveredBatchesDiscardsRealWriterPrefix)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid()); const auto target = dir.filePath("output.cache");
    Source source(QByteArray("1 2 0\n").repeated(9000)); source.block = 1024; source.failure = 48000;
    int batches = 0;
    {
        auto created = CacheWriter::create(target, {8ULL << 20, 64ULL << 20, 5}, dir.path()); ASSERT_TRUE(created);
        auto writer = std::move(created.value());
        auto parsed = parseAscXyz(source, [&](auto batch) { ++batches; return writer->append(batch); }, {});
        ASSERT_FALSE(parsed); EXPECT_EQ(parsed.error().code, ErrorCode::ReadFailed);
        EXPECT_GT(batches, 0);
    }
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
}
