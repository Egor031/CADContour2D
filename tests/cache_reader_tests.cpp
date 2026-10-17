#include "cache/cache_reader.h"
#include "cache/cache_writer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>
#include <stdexcept>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace cadcontour;
using namespace cadcontour::cache;

namespace cadcontour::cache {
class CacheReaderTestAccess {
public:
    static void hook(CacheReader &r, Status (*fn)(CacheReader &, Operation)) { r.ioHook_ = fn; }
    static QFile &file(CacheReader &r) { return r.file_; }
    static auto statistics(const CacheReader &r) { return r.statistics_; }
    static auto reserve(const CacheReader &r) { return r.reserve_; }
    static Status shape(std::uint64_t pointPages, std::uint32_t height, std::uint64_t indexPages) {
        CacheReader r(QString{});
        r.layout_.pointPages = pointPages;
        r.header_.index = {IndexKind::BoundingBoxTree, indexPages, indexPages - 1, height};
        return r.shape();
    }
    static auto buffers(const CacheReader &r) {
        return (r.page_ ? format::pageSize : 0) + (r.records_ ? format::pointCapacity * sizeof(PointRecord) : 0)
            + (r.frames_ ? r.header_.index.height * sizeof(CacheReader::Frame) : 0);
    }
};
} // namespace cadcontour::cache

namespace {
using R = CacheReaderTestAccess;
constexpr ResourceLimits readLimits{1024 * 1024, 0, 1};
constexpr ResourceLimits validationLimits{2 * 1024 * 1024, 512 * 1024 * 1024, 3};
constexpr ResourceLimits writeLimits{4 * 1024 * 1024, 512 * 1024 * 1024, 4};
auto bits(double d) { return std::bit_cast<std::uint64_t>(d); }
Status discard(std::span<const PointRecord>) { return success(); }
bool inside(const Measurement &m, BoundingBox b) {
    return m.x >= b.minX && m.x <= b.maxX && m.y >= b.minY && m.y <= b.maxY;
}
std::vector<Measurement> data(std::size_t n)
{
    const std::array<double, 14> values{-std::numeric_limits<double>::max(), -1000, -1,
        std::nextafter(-1., -2.), -std::numeric_limits<double>::min(),
        -std::numeric_limits<double>::denorm_min(), -0., +0., std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::min(), 1, std::nextafter(1., 2.), 1000, std::numeric_limits<double>::max()};
    std::vector<Measurement> result;
    for (std::size_t i = 0; i < n; ++i)
        result.push_back({values[i % values.size()], values[(i / values.size()) % values.size()], i % 13 ? 0. : 3.});
    return result;
}
Result<PendingCache> write(const QString &path, const QString &parent, std::span<const Measurement> input)
{
    auto w = CacheWriter::create(path, writeLimits, parent);
    if (!w) return w.error();
    if (auto s = w.value()->append(input); !s) return s.error();
    return w.value()->finish(); // Writer is destroyed before the reader is constructed.
}
void same(const std::vector<PointRecord> &records, const std::vector<Measurement> &input,
          std::span<const BoundingBox> boxes, bool all = false)
{
    std::vector<bool> seen(input.size()); // Test oracle only, never production state.
    for (const auto &r : records) {
        ASSERT_LT(r.id, input.size());
        EXPECT_FALSE(seen[static_cast<std::size_t>(r.id)]);
        seen[static_cast<std::size_t>(r.id)] = true;
        EXPECT_EQ(bits(r.point.x), bits(input[static_cast<std::size_t>(r.id)].x));
        EXPECT_EQ(bits(r.point.y), bits(input[static_cast<std::size_t>(r.id)].y));
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        bool expected = all;
        for (auto b : boxes) expected |= inside(input[i], b);
        EXPECT_EQ(seen[i], expected) << i;
    }
}
void mutate(const QString &path, std::uint64_t offset, std::size_t size,
            const std::function<void(MutableBytes)> &change, bool hash = true)
{
    QFile f(path); ASSERT_TRUE(f.open(QIODevice::ReadWrite | QIODevice::Unbuffered));
    ASSERT_TRUE(file::seek(f, offset));
    std::vector<std::byte> bytes(size);
    ASSERT_TRUE(file::readExact(f, bytes));
    change(bytes);
    if (hash) ASSERT_TRUE(sealChecksum(bytes));
    ASSERT_TRUE(file::seek(f, offset));
    ASSERT_TRUE(file::writeExact(f, bytes));
    ASSERT_TRUE(f.flush());
}
class ReaderRoundTrip : public testing::TestWithParam<std::size_t> {};
TEST_P(ReaderRoundTrip, BitsIdentityMetadataAndValidation)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    auto input = data(GetParam());
    auto cache = write(dir.filePath("roundtrip"), dir.path(), input); ASSERT_TRUE(cache);
    const auto meta = cache.value().metadata();
    auto opened = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(opened);
    auto reader = std::move(opened.value());
    EXPECT_EQ(R::statistics(*reader).pointPages, 0);
    EXPECT_EQ(R::statistics(*reader).indexPages, input.empty() ? 0 : 1);
    EXPECT_EQ(R::statistics(*reader).readBytes,
        format::headerSize + format::footerSize + (input.empty() ? 0 : format::pageSize));
    EXPECT_EQ(reader->metadata().id, meta.id);
    EXPECT_EQ(reader->metadata().pointCount, input.size());
    EXPECT_EQ(reader->metadata().hasNonZeroZ, meta.hasNonZeroZ);
    EXPECT_EQ(bits(reader->metadata().bounds.minX), bits(meta.bounds.minX));
    EXPECT_EQ(bits(reader->metadata().bounds.minY), bits(meta.bounds.minY));
    EXPECT_EQ(bits(reader->metadata().bounds.maxX), bits(meta.bounds.maxX));
    EXPECT_EQ(bits(reader->metadata().bounds.maxY), bits(meta.bounds.maxY));
    std::vector<PointRecord> records;
    auto scan = reader->scanAll([&](auto batch) {
        EXPECT_LE(batch.size(), format::pointCapacity);
        records.insert(records.end(), batch.begin(), batch.end()); return success();
    });
    ASSERT_TRUE(scan); same(records, input, {}, true);
    EXPECT_EQ(scan.value().pointPages, reader->layout().pointPages);
    EXPECT_EQ(scan.value().indexPages, 0);
    EXPECT_EQ(scan.value().bufferBytes, R::buffers(*reader));
    // A second, independent handle can coexist with PendingCache, without Accept.
    auto second = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(second);
    EXPECT_TRUE(second.value()->scanAll(discard));
    const auto before = QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    auto validated = reader->validate(validationLimits, dir.path()); ASSERT_TRUE(validated);
    EXPECT_EQ(validated.value().pointPages, reader->layout().pointPages);
    EXPECT_EQ(validated.value().indexPages, reader->index().pageCount);
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), before);
    reader.reset();
    EXPECT_TRUE(QFileInfo::exists(cache.value().path()));
}
INSTANTIATE_TEST_SUITE_P(PageBoundaries, ReaderRoundTrip,
    testing::Values(std::size_t{0}, std::size_t{1}, std::size_t{271}, format::pointCapacity,
                    format::pointCapacity + 1, std::size_t{12000}));

TEST(CacheReader, SpatialUnionMatchesIndependentOracleAndPhysicalOrder)
{
    QTemporaryDir dir;
    const auto input = data(15000);
    auto cache = write(dir.filePath("queries"), dir.path(), input); ASSERT_TRUE(cache);
    auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
    std::vector<PointRecord> physical;
    ASSERT_TRUE(r.value()->scanAll([&](auto b) { physical.insert(physical.end(), b.begin(), b.end()); return success(); }));
    const double denorm = std::numeric_limits<double>::denorm_min();
    const double max = std::numeric_limits<double>::max();
    const std::vector<std::vector<BoundingBox>> queries{
        {}, {{-max, -max, max, max}}, {{-0., -0., +0., +0.}}, {{1, -max, 1, max}},
        {{-max, -1, max, -1}}, {{-denorm, -denorm, denorm, denorm}},
        {{1, 1, std::nextafter(1., 2.), std::nextafter(1., 2.)}}, {{2000, 2000, 3000, 3000}},
        {{-1, -1, 1, 1}, {-1, -1, 1, 1}, {0, 0, 1000, 1000}},
        {{-1000, -1000, -1, -1}, {1, 1, 1000, 1000}}};
    for (const auto &boxes : queries) {
        std::vector<PointRecord> out;
        auto sink = [&](auto b) { out.insert(out.end(), b.begin(), b.end()); return success(); };
        auto result = r.value()->scanBoxes(boxes, sink); ASSERT_TRUE(result);
        same(out, input, boxes);
        EXPECT_LE(result.value().pointPages, r.value()->layout().pointPages);
        std::vector<RecordId> expected, actual;
        for (const auto &p : physical) {
            for (auto box : boxes) {
                if (inside({p.point.x, p.point.y, 0}, box)) { expected.push_back(p.id); break; }
            }
        }
        for (const auto &p : out) actual.push_back(p.id);
        EXPECT_EQ(actual, expected);
        if (boxes.size() == 1) {
            out.clear(); ASSERT_TRUE(r.value()->scanBox(boxes.front(), sink)); same(out, input, boxes);
        }
    }
}

TEST(CacheReader, InvalidQueriesAreRejectedBeforeIOOrOutput)
{
    QTemporaryDir dir;
    auto cache = write(dir.filePath("invalid"), dir.path(), data(1)); ASSERT_TRUE(cache);
    auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (const auto b : std::vector<BoundingBox>{{1, 0, 0, 1}, {0, 1, 1, 0}, {nan, 0, 1, 1},
             {0, nan, 1, 1}, {0, 0, nan, 1}, {0, 0, 1, nan}, {-inf, 0, 1, 1}, {0, 0, inf, 1},
             {0, -inf, 1, 1}, {0, 0, 1, inf}}) {
        const std::array<BoundingBox, 2> boxes{{{-1, -1, 1, 1}, b}};
        auto result = r.value()->scanBoxes(boxes, [](auto) -> Status { ADD_FAILURE(); return success(); });
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
        EXPECT_EQ(R::statistics(*r.value()).readOperations, 0);
    }
    EXPECT_FALSE(r.value()->scanAll({}));
}

class ReaderCorruption : public testing::TestWithParam<int> {};
TEST_P(ReaderCorruption, RehashedSemanticFailuresAndLazyBoundary)
{
    QTemporaryDir dir;
    std::vector<Measurement> input;
    for (int i = 0; i < 6000; ++i) input.push_back({double(i), double(i), 0});
    auto cache = write(dir.filePath("corrupt"), dir.path(), input); ASSERT_TRUE(cache);
    const auto l = cache.value().layout();
    const auto index = l.indexOffset;
    const auto point = format::headerSize;
    const int mode = GetParam();
    bool openFails = mode < 18;
    switch (mode) {
    case 0: mutate(cache.value().path(), 0, format::subblockSize, [](auto b) { b[0] ^= std::byte{1}; }, false); break;
    case 1: mutate(cache.value().path(), format::subblockSize, format::subblockSize, [](auto b) { b[0] ^= std::byte{1}; }, false); break;
    case 2: mutate(cache.value().path(), l.footerOffset, format::footerSize, [](auto b) { b[0] ^= std::byte{1}; }, false); break;
    case 3: mutate(cache.value().path(), index, format::pageSize, [](auto b) { b[0] ^= std::byte{1}; }, false); break;
    case 4: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putU64(b, 24, 1); }); break;
    case 5: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putU32(b, 32, 2); }); break;
    case 6: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putU32(b, 12, 1); }); break;
    case 7: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putU32(b, 20, 2); }); break;
    case 8: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putU64(b, 64 + 32, 99); }); break;
    case 9: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putU64(b, 64 + 64 + 32, 0); }); break;
    case 10: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putU64(b, 64 + 40, 100); }); break;
    case 11: mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putDouble(b, 64, -1); }); break;
    case 12: mutate(cache.value().path(), format::subblockSize, format::subblockSize, [](auto b) { binary::putU64(b, 48, 1); }); break;
    case 13: mutate(cache.value().path(), format::subblockSize, format::subblockSize, [](auto b) { binary::putU32(b, 56, UINT32_MAX); }); break;
    case 14: mutate(cache.value().path(), 0, format::subblockSize, [](auto b) { binary::putDouble(b, 80, 6000); }); break;
    case 15: mutate(cache.value().path(), 0, format::subblockSize, [](auto b) { binary::putU32(b, 8, 2); }); break;
    case 16: mutate(cache.value().path(), l.footerOffset, format::footerSize, [](auto b) { binary::putU32(b, 12, 0); }); break;
    case 17: mutate(cache.value().path(), l.footerOffset, format::footerSize, [](auto b) { binary::putU64(b, 32, 5999); }); break;
    case 18: mutate(cache.value().path(), point, format::pageSize, [](auto b) { b[100] ^= std::byte{1}; }, false); break;
    case 19: mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putU64(b, 24, 1); }); break;
    case 20: mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putU32(b, 32, 1); }); break;
    case 21: mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putU64(b, 64, 6000); }); break;
    case 22: // Duplicate ID, leaving a hole. Still legal as a local point record.
        mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putU64(b, 64 + 24, *binary::getU64(b, 64)); }); break;
    case 23: // Interior coordinate changes bounds of the first point page only.
        mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putDouble(b, 64 + 8, 0.5); }); break;
    case 24: // Corrupt a page that an unrelated spatial query prunes.
        mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putDouble(b, 64 + 16, -1); }); break;
    case 25: // Incorrect dataset bounds consistent with root, requiring real points to disprove.
        mutate(cache.value().path(), 0, format::subblockSize, [](auto b) { binary::putDouble(b, 64, -1); });
        mutate(cache.value().path(), index, format::pageSize, [](auto b) { binary::putDouble(b, 64, -1); }); break;
    case 26: mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putU32(b, 20, 0); }); break;
    case 27: mutate(cache.value().path(), point, format::pageSize, [](auto b) { binary::putDouble(b, 72, std::numeric_limits<double>::infinity()); }); break;
    default: FAIL();
    }
    auto r = CacheReader::open(cache.value().path(), readLimits);
    if (openFails) {
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, mode == 15 ? ErrorCode::UnsupportedFormat
            : mode == 16 ? ErrorCode::IncompleteCache : ErrorCode::CorruptCache);
        return;
    }
    ASSERT_TRUE(r);
    if (mode == 24) EXPECT_TRUE(r.value()->scanBox({5999, 5999, 5999, 5999}, discard));
    auto scan = r.value()->scanAll(discard);
    if (mode == 22 || mode == 23 || mode == 25) EXPECT_TRUE(scan); else EXPECT_FALSE(scan);
    if (mode != 22) EXPECT_FALSE(r.value()->scanBox({0, 0, 5999, 5999}, discard));
    auto v = r.value()->validate(validationLimits, dir.path());
    ASSERT_FALSE(v); EXPECT_EQ(v.error().code, ErrorCode::CorruptCache);
    EXPECT_TRUE(QFileInfo::exists(cache.value().path()));
}
INSTANTIATE_TEST_SUITE_P(ChecksumsAndSemantics, ReaderCorruption, testing::Range(0, 28));

TEST(CacheReader, TruncationAndExtraBytes)
{
    QTemporaryDir dir;
    for (const auto cut : std::array<qint64, 5>{0, 100, 4095, 5000, 2 * 65536 + 8191}) {
        auto cache = write(dir.filePath(QString::number(cut)), dir.path(), data(1)); ASSERT_TRUE(cache);
        QFile f(cache.value().path()); ASSERT_TRUE(f.open(QIODevice::ReadWrite)); ASSERT_TRUE(f.resize(cut)); f.close();
        auto r = CacheReader::open(cache.value().path(), readLimits);
        ASSERT_FALSE(r); EXPECT_EQ(r.error().code, ErrorCode::IncompleteCache);
    }
    auto cache = write(dir.filePath("extra"), dir.path(), data(1)); ASSERT_TRUE(cache);
    QFile f(cache.value().path()); ASSERT_TRUE(f.open(QIODevice::Append)); ASSERT_EQ(f.write("x"), 1); f.close();
    EXPECT_FALSE(CacheReader::open(cache.value().path(), readLimits));
    EXPECT_FALSE(CacheReader::open(dir.filePath("missing"), readLimits));
}

TEST(CacheReader, CallbackFailureExceptionsCancellationAndReentryPermitReuse)
{
    QTemporaryDir dir;
    auto cache = write(dir.filePath("faults"), dir.path(), data(7000)); ASSERT_TRUE(cache);
    QFile source(cache.value().path()); ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto originalBytes = source.readAll(); source.close();
    auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
    const auto failure = makeError(ErrorCode::WriteFailed, Phase::Output, Operation::Write, QStringLiteral("sink failure"), 42);
    auto failed = r.value()->scanAll([&](auto) -> Status { return failure; });
    ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code, failure.code); EXPECT_EQ(failed.error().offset, 42);
    auto thrown = r.value()->scanAll([](auto) -> Status { throw std::runtime_error("consumer"); });
    ASSERT_FALSE(thrown); EXPECT_EQ(thrown.error().code, ErrorCode::ConsumerFailed);
    auto alloc = r.value()->scanAll([](auto) -> Status { throw std::bad_alloc{}; });
    ASSERT_FALSE(alloc); EXPECT_EQ(alloc.error().code, ErrorCode::ConsumerFailed);
    bool cancel = true;
    auto check = [](void *p) noexcept { return *static_cast<bool *>(p); };
    EXPECT_FALSE(r.value()->scanAll(discard, check, &cancel));
    EXPECT_EQ(R::statistics(*r.value()).pointPages, 0);
    EXPECT_FALSE(CacheReader::open(cache.value().path(), readLimits, check, &cancel));
    cancel = false;
    std::size_t emitted = 0;
    auto partial = r.value()->scanAll([&](auto b) { emitted += b.size(); cancel = true; return success(); }, check, &cancel);
    ASSERT_FALSE(partial); EXPECT_EQ(partial.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(emitted, format::pointCapacity);
    auto nested = r.value()->scanAll([&](auto) {
        auto inner = r.value()->scanAll(discard); EXPECT_FALSE(inner);
        EXPECT_EQ(inner.error().code, ErrorCode::InvalidState);
        auto v = r.value()->validate(validationLimits, dir.path()); EXPECT_FALSE(v);
        EXPECT_EQ(v.error().code, ErrorCode::InvalidState);
        // A call from another thread is rejected before touching the handle/buffers.
        auto concurrent = std::async(std::launch::async, [&] { return r.value()->scanAll(discard); }).get();
        EXPECT_FALSE(concurrent); EXPECT_EQ(concurrent.error().code, ErrorCode::InvalidState);
        return success();
    });
    ASSERT_TRUE(nested); EXPECT_EQ(nested.value().returnedRecords, 7000);
    EXPECT_TRUE(r.value()->scanAll(discard));
    EXPECT_TRUE(r.value()->validate(validationLimits, dir.path()));
    ASSERT_TRUE(source.open(QIODevice::ReadOnly)); EXPECT_EQ(source.readAll(), originalBytes);
}

TEST(CacheReader, ReadSeekAndTruncationFaultsUseProductionIO)
{
    QTemporaryDir dir;
    auto cache = write(dir.filePath("io"), dir.path(), data(3000)); ASSERT_TRUE(cache);
    auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
    R::hook(*r.value(), [](CacheReader &, Operation op) -> Status {
        if (op == Operation::Seek) return makeError(ErrorCode::SeekFailed, Phase::FileIO, op, QStringLiteral("injected seek"));
        return success();
    });
    auto seek = r.value()->scanAll(discard); ASSERT_FALSE(seek); EXPECT_EQ(seek.error().code, ErrorCode::SeekFailed);
    R::hook(*r.value(), [](CacheReader &, Operation op) -> Status {
        if (op == Operation::Read) return makeError(ErrorCode::ReadFailed, Phase::FileIO, op, QStringLiteral("injected read"));
        return success();
    });
    auto read = r.value()->scanAll(discard); ASSERT_FALSE(read); EXPECT_EQ(read.error().code, ErrorCode::ReadFailed);
    R::hook(*r.value(), nullptr);
    EXPECT_TRUE(r.value()->scanAll(discard));
    R::hook(*r.value(), [](CacheReader &, Operation) -> Status { throw std::bad_alloc{}; });
    auto allocation = r.value()->scanAll(discard);
    ASSERT_FALSE(allocation); EXPECT_EQ(allocation.error().code, ErrorCode::ResourceExhausted);
    R::hook(*r.value(), nullptr);
    EXPECT_TRUE(r.value()->scanAll(discard));
    // Mutation by a separate handle deliberately violates the immutable-file contract.
    auto truncated = r.value()->scanAll([&](auto) -> Status {
        QFile f(cache.value().path());
        EXPECT_TRUE(f.open(QIODevice::ReadWrite)); EXPECT_TRUE(f.resize(format::headerSize + format::pageSize));
        return success();
    });
    ASSERT_FALSE(truncated); EXPECT_EQ(truncated.error().code, ErrorCode::IncompleteCache);
    EXPECT_TRUE(QFileInfo::exists(cache.value().path()));
}

TEST(CacheReader, ValidationBudgetsCancellationAndCleanup)
{
    QTemporaryDir dir;
    auto cache = write(dir.filePath("budgets"), dir.path(), data(10000)); ASSERT_TRUE(cache);
    EXPECT_FALSE(CacheReader::open(cache.value().path(), {1, 0, 1}));
    EXPECT_FALSE(CacheReader::open(cache.value().path(), {0, 0, 0}));
    auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
    const auto entries = QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (auto limits : std::array<ResourceLimits, 3>{{{1, 1000000, 3}, {2000000, 1, 3}, {2000000, 1000000, 2}}}) {
        auto v = r.value()->validate(limits, dir.path()); ASSERT_FALSE(v);
        EXPECT_EQ(v.error().code, ErrorCode::ResourceExhausted);
    }
    // Enough budget to create runs, insufficient for subsequent growth.
    auto disk = r.value()->validate({500000, 10000, 3}, dir.path()); EXPECT_FALSE(disk);
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), entries);
    int calls = 0;
    auto cancelled = r.value()->validate(validationLimits, dir.path(), [](void *p) noexcept {
        return ++*static_cast<int *>(p) > 60;
    }, &calls);
    ASSERT_FALSE(cancelled); EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
    EXPECT_GT(calls, 60);
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), entries);
    EXPECT_TRUE(r.value()->validate(validationLimits, dir.path()));
}

TEST(CacheReader, IDValidationRejectsEqualSumXorAndExtremaAndCrossPageDuplicates)
{
    QTemporaryDir dir;
    // 0,1,2,3,4,5 -> 0,0,3,3,4,5 preserves count, sum, XOR and min/max.
    std::vector<Measurement> sample;
    for (int i = 0; i < 6; ++i) sample.push_back({double(i), double(i), 0});
    auto cache = write(dir.filePath("aggregates"), dir.path(), sample); ASSERT_TRUE(cache);
    mutate(cache.value().path(), format::headerSize, format::pageSize, [](auto b) {
        binary::putU64(b, 64 + 24, 0); binary::putU64(b, 64 + 48, 3);
    });
    auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
    EXPECT_TRUE(r.value()->scanAll(discard));
    auto v = r.value()->validate(validationLimits, dir.path()); ASSERT_FALSE(v);
    EXPECT_EQ(v.error().code, ErrorCode::CorruptCache);

    std::vector<Measurement> many;
    for (int i = 0; i < 12000; ++i) many.push_back({double(i), double(i), 0});
    auto cross = write(dir.filePath("cross-page"), dir.path(), many); ASSERT_TRUE(cross);
    mutate(cross.value().path(), format::headerSize + 3 * format::pageSize, format::pageSize,
        [](auto b) { binary::putU64(b, 64, 0); });
    auto reader = CacheReader::open(cross.value().path(), readLimits); ASSERT_TRUE(reader);
    EXPECT_TRUE(reader.value()->scanAll(discard));
    EXPECT_FALSE(reader.value()->validate({500000, 10000000, 3}, dir.path()));
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}

TEST(CacheReader, ValidationCancellationDuringIDSortAndAllocationFailure)
{
    QTemporaryDir dir;
    auto cache = write(dir.filePath("cancel-sort"), dir.path(), data(10000)); ASSERT_TRUE(cache);
    auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
    struct Context { CacheReader *reader; int afterWalk = 0; } context{r.value().get()};
    auto cancelled = r.value()->validate(validationLimits, dir.path(), [](void *p) noexcept {
        auto &c = *static_cast<Context *>(p);
        if (R::statistics(*c.reader).returnedRecords == c.reader->metadata().pointCount) return ++c.afterWalk > 5;
        return false;
    }, &context);
    ASSERT_FALSE(cancelled); EXPECT_EQ(cancelled.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(R::statistics(*r.value()).pointPages, r.value()->layout().pointPages);
    EXPECT_GT(context.afterWalk, 5);
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    R::hook(*r.value(), [](CacheReader &, Operation) -> Status { throw std::bad_alloc{}; });
    auto allocation = r.value()->validate(validationLimits, dir.path()); ASSERT_FALSE(allocation);
    EXPECT_EQ(allocation.error().code, ErrorCode::ResourceExhausted);
    R::hook(*r.value(), nullptr);
    EXPECT_TRUE(r.value()->validate(validationLimits, dir.path()));
}

TEST(CacheReader, HigherPackedLevelsHaveBoundedShapeAndRejectMaliciousHeight)
{
    constexpr std::uint64_t c = format::indexCapacity;
    EXPECT_TRUE(R::shape(c * c + 1, 3, c + 1 + 2 + 1));
    EXPECT_TRUE(R::shape(c * c * c + 1, 4, c * c + 1 + c + 1 + 2 + 1));
    EXPECT_FALSE(R::shape(c * c + 1, UINT32_MAX, c + 5));
    EXPECT_FALSE(R::shape(c * c + 1, 3, UINT64_MAX));
}

TEST(CacheReader, HandlesAndBuffersAreBoundedAndReleased)
{
    QTemporaryDir dir;
    auto cache = write(dir.filePath("handles"), dir.path(), data(30000)); ASSERT_TRUE(cache);
    DWORD before = 0, during = 0, after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
    {
        auto r = CacheReader::open(cache.value().path(), readLimits); ASSERT_TRUE(r);
        ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &during)); EXPECT_EQ(during, before + 1);
        const auto buffers = R::buffers(*r.value());
        EXPECT_LT(buffers, readLimits.memoryBytes);
        ASSERT_TRUE(r.value()->scanAll([&](auto b) {
            EXPECT_LE(b.size(), format::pointCapacity);
            EXPECT_EQ(R::buffers(*r.value()), buffers);
            return success();
        }));
        ASSERT_TRUE(r.value()->validate(validationLimits, dir.path()));
        ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &during)); EXPECT_EQ(during, before + 1);
        EXPECT_EQ(R::buffers(*r.value()), buffers);
    }
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after)); EXPECT_EQ(after, before);
}

// Explicit integration label: not part of the normal production regression.
TEST(CacheReaderIntegration, RealWriterHeightTwoAndSpatialIOMeasurements)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("height-two");
    constexpr std::uint64_t n = format::indexCapacity * format::pointCapacity + 37;
    auto measurement = [](std::uint64_t i) -> Measurement {
        // Alternating horizontal/vertical passes; both dense rows and sparse jumps.
        const auto j = i / 2;
        return i % 2 ? Measurement{double(j / 2048), double(j % 2048), 0}
                     : Measurement{double(j % 2048), double(j / 2048), 0};
    };
    auto w = CacheWriter::create(path, writeLimits, dir.path()); ASSERT_TRUE(w);
    std::array<Measurement, 4096> batch;
    for (std::uint64_t first = 0; first < n;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(batch.size(), n - first));
        for (std::size_t i = 0; i < count; ++i) batch[i] = measurement(first + i);
        ASSERT_TRUE(w.value()->append({batch.data(), count})); first += count;
    }
    auto cache = w.value()->finish(); ASSERT_TRUE(cache); w.value().reset();
    ASSERT_EQ(cache.value().index().height, 2);
    auto r = CacheReader::open(path, readLimits); ASSERT_TRUE(r);
    ASSERT_EQ(r.value()->layout().pointPages, 1023);
    const std::vector<std::vector<BoundingBox>> queries{
        {{10, 10, 15, 15}}, {{100, 0, 100, 2047}},
        {{10, 10, 30, 30}, {20, 20, 40, 40}, {10, 10, 30, 30}}, {}};
    const auto buffers = R::buffers(*r.value());
    for (std::size_t q = 0; q < queries.size(); ++q) {
        const auto &boxes = queries[q];
        const bool all = q + 1 == queries.size();
        std::vector<bool> seen(static_cast<std::size_t>(n));
        std::uint64_t returned = 0;
        QElapsedTimer timer; timer.start();
        auto sink = [&](auto records) -> Status {
            for (const auto &p : records) {
                if (p.id >= n) { ADD_FAILURE(); return makeError(ErrorCode::ConsumerFailed, Phase::Output, Operation::Consume, "bad id"); }
                EXPECT_FALSE(seen[static_cast<std::size_t>(p.id)]); seen[static_cast<std::size_t>(p.id)] = true;
                const auto original = measurement(p.id);
                EXPECT_EQ(bits(p.point.x), bits(original.x)); EXPECT_EQ(bits(p.point.y), bits(original.y));
                ++returned;
            }
            EXPECT_EQ(R::buffers(*r.value()), buffers);
            return success();
        };
        auto result = all ? r.value()->scanAll(sink) : r.value()->scanBoxes(boxes, sink);
        const auto elapsed = timer.nsecsElapsed(); ASSERT_TRUE(result);
        std::uint64_t oracle = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            bool expected = all;
            for (const auto box : boxes) expected |= inside(measurement(i), box);
            if (seen[static_cast<std::size_t>(i)] != expected) { ADD_FAILURE() << "ID " << i; break; }
            oracle += expected;
        }
        EXPECT_EQ(returned, oracle);
        const auto &s = result.value();
        if (!all) EXPECT_LT(s.pointPages, 1023); else EXPECT_EQ(s.pointPages, 1023);
        EXPECT_EQ(s.traversalFrames, 2);
        std::printf("IO q=%zu N=%llu file=%llu index=%llu point=%llu bytes=%llu reads=%llu seeks=%llu returned=%llu ms=%.3f buffers=%llu\n",
            q, static_cast<unsigned long long>(n), static_cast<unsigned long long>(r.value()->layout().fileSize),
            static_cast<unsigned long long>(s.indexPages), static_cast<unsigned long long>(s.pointPages),
            static_cast<unsigned long long>(s.readBytes), static_cast<unsigned long long>(s.readOperations),
            static_cast<unsigned long long>(s.seekOperations), static_cast<unsigned long long>(returned), elapsed / 1.e6,
            static_cast<unsigned long long>(buffers));
        if (!all) {
            std::uint64_t selected = 0;
            timer.restart();
            auto sequential = r.value()->scanAll([&](auto records) -> Status {
                for (const auto &p : records) {
                    bool hit = false;
                    for (const auto box : boxes) hit |= inside({p.point.x, p.point.y, 0}, box);
                    if (hit) {
                        if (!seen[static_cast<std::size_t>(p.id)]) ADD_FAILURE() << "Spatial/sequence mismatch";
                        ++selected;
                    }
                }
                return success();
            });
            const auto sequentialTime = timer.nsecsElapsed(); ASSERT_TRUE(sequential);
            EXPECT_EQ(selected, returned);
            const auto &ss = sequential.value();
            std::printf("SEQ q=%zu index=%llu point=%llu bytes=%llu reads=%llu seeks=%llu selected=%llu ms=%.3f\n",
                q, static_cast<unsigned long long>(ss.indexPages), static_cast<unsigned long long>(ss.pointPages),
                static_cast<unsigned long long>(ss.readBytes), static_cast<unsigned long long>(ss.readOperations),
                static_cast<unsigned long long>(ss.seekOperations), static_cast<unsigned long long>(selected), sequentialTime / 1.e6);
        }
    }
    ASSERT_TRUE(r.value()->validate(validationLimits, dir.path()));
    // Non-root corruption stays lazy; full validation and a relevant query reject it.
    QFile original(path); ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    ASSERT_TRUE(file::seek(original, r.value()->layout().indexOffset));
    const auto saved = original.read(format::pageSize); ASSERT_EQ(saved.size(), format::pageSize); original.close();
    for (int mode = 0; mode < 8; ++mode) {
        mutate(path, r.value()->layout().indexOffset, format::pageSize, [&](auto b) {
            std::copy_n(reinterpret_cast<const std::byte *>(saved.constData()), b.size(), b.begin());
            switch (mode) {
            case 0: binary::putU32(b, 32, 2); break;
            case 1: binary::putU64(b, 24, 1); break;
            case 2: binary::putU64(b, 64 + 32, format::noPage); break;
            case 3: binary::putU64(b, 64 + 64 + 32, 0); break;
            case 4: binary::putU64(b, 64 + 32, 1); break;
            case 5: binary::putU64(b, 64 + 40, 1); break;
            case 6: binary::putU32(b, 64 + 48, 2); break;
            case 7: binary::putDouble(b, 64, -1); break;
            }
        });
        auto reopened = CacheReader::open(path, readLimits); ASSERT_TRUE(reopened);
        EXPECT_FALSE(reopened.value()->scanBox({10, 10, 15, 15}, discard)) << mode;
        EXPECT_FALSE(reopened.value()->validate(validationLimits, dir.path())) << mode;
    }
}
} // namespace
