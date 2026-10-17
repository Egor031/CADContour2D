#include "point_stream_sort_test_access.h"

#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace cadcontour;
using namespace cadcontour::cache;

namespace {
using Access = PointStreamSortTestAccess;
std::uint64_t bits(double v) { return std::bit_cast<std::uint64_t>(v); }

// Independent ordering oracle: choose the axis with the highest differing IEEE
// bit, then compare its coordinates numerically. No monotone transform, bit
// interleaving, or call to the production Morton function. X wins equal levels.
bool oracleLess(const PointRecord &a, const PointRecord &b)
{
    auto canonical = [](double v) { return bits(v == 0 ? 0. : v); };
    const auto dx = std::bit_width(canonical(a.point.x) ^ canonical(b.point.x));
    const auto dy = std::bit_width(canonical(a.point.y) ^ canonical(b.point.y));
    if (!dx && !dy) return a.id < b.id;
    return dx >= dy ? a.point.x < b.point.x : a.point.y < b.point.y;
}
std::vector<Measurement> measurements(std::size_t n)
{
    const std::array<double, 16> values{-std::numeric_limits<double>::max(), -1e100, -1024, -1,
        std::nextafter(-1., -2.), -std::numeric_limits<double>::min(),
        -std::numeric_limits<double>::denorm_min(), -0., 0.,
        std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min(),
        std::nextafter(1., 2.), 1, 1024, 1e100, std::numeric_limits<double>::max()};
    std::mt19937 random(48327);
    std::vector<Measurement> result;
    for (std::size_t i = 0; i < n; ++i)
        result.push_back({values[random() % values.size()], values[random() % values.size()], i % 5 == 0 ? 2. : -0.});
    return result;
}
class CacheStreamSort : public ::testing::Test {
protected:
    QTemporaryDir parent;
    ResourceLimits limits(std::uint64_t extra = 400) const
    {
        // Match the documented fixed/path reserve, leaving a deliberately tiny workspace.
        return {65536 + static_cast<std::uint64_t>(QDir(parent.path()).absolutePath().size()) * 16 + extra,
                64 * 1024 * 1024, 2};
    }
    std::unique_ptr<PointStreamSort> create(std::uint64_t extra = 400,
                                           PointStreamSort::CancelCheck cancel = nullptr, void *context = nullptr)
    {
        auto result = PointStreamSort::create(limits(extra), parent.path(), cancel, context);
        EXPECT_TRUE(result) << (result ? "" : result.error().detail.toStdString());
        return result ? std::move(result.value()) : nullptr;
    }
    void expectClean(const PointStreamSort &s)
    {
        EXPECT_TRUE(QDir(parent.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
        EXPECT_EQ(Access::stats(s).liveBufferBytes, 0);
        EXPECT_EQ(Access::stats(s).openFiles, 0);
    }
    void roundTrip(std::vector<Measurement> input, std::size_t batchSize, std::uint64_t extra = 400,
                   bool multiGeneration = false)
    {
        auto sorter = create(extra); ASSERT_TRUE(sorter);
        std::vector<PointRecord> expected, output;
        for (std::size_t i = 0; i < input.size(); ++i) expected.push_back({i, {input[i].x, input[i].y}});
        std::sort(expected.begin(), expected.end(), oracleLess);
        ASSERT_TRUE(sorter->append({}));
        for (std::size_t i = 0; i < input.size(); i += batchSize)
            ASSERT_TRUE(sorter->append(std::span(input).subspan(i, std::min(batchSize, input.size() - i))));
        const auto callsBefore = Access::stats(*sorter).initialRuns;
        auto result = sorter->finish([&](std::span<const PointRecord> block) {
            EXPECT_FALSE(block.empty()); EXPECT_LE(block.size(), Access::block(*sorter));
            output.insert(output.end(), block.begin(), block.end()); return success();
        });
        ASSERT_TRUE(result) << result.error().detail.toStdString();
        EXPECT_EQ(result.value().pointCount, input.size());
        EXPECT_FALSE(result.value().id.isNull());
        ASSERT_EQ(output.size(), expected.size());
        for (std::size_t i = 0; i < output.size(); ++i) {
            EXPECT_EQ(output[i].id, expected[i].id) << i;
            EXPECT_EQ(bits(output[i].point.x), bits(expected[i].point.x)) << i;
            EXPECT_EQ(bits(output[i].point.y), bits(expected[i].point.y)) << i;
        }
        const auto stats = Access::stats(*sorter);
        EXPECT_LE(stats.peakBufferBytes + Access::fixed(*sorter), limits(extra).memoryBytes);
        EXPECT_LE(stats.peakOpenFiles, limits(extra).maxOpenFiles);
        EXPECT_LE(stats.peakPlannedDiskBytes, limits(extra).temporaryDiskBytes);
        if (multiGeneration) {
            EXPECT_GT(callsBefore, Access::fan(*sorter));
            EXPECT_GE(stats.mergeGenerations, 2);
            EXPECT_EQ(stats.peakOpenFiles, 2);
        }
        expectClean(*sorter);
        EXPECT_FALSE(sorter->append({}));
        EXPECT_FALSE(sorter->finish([](auto) { return success(); }));
    }
};

TEST(CacheMorton, IndependentGoldenWords)
{
    struct Golden { ModelPoint point; Morton128 key; };
    const std::array<Golden, 9> golden{{
        {{-1, 0}, {0x600000aaaaaaaaaaULL, 0xaaaaaaaaaaaaaaaaULL}},
        {{0, -1}, {0x9000005555555555ULL, 0x5555555555555555ULL}},
        {{1, 0}, {0xcaaaaa0000000000ULL, 0}},
        {{0, 1}, {0xc555550000000000ULL, 0}},
        {{-0., 0}, {0xc000000000000000ULL, 0}},
        {{std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min()},
         {0x9555555555555555ULL, 0x5555555555555556ULL}},
        {{std::nextafter(1., 2.), 1}, {0xcfffff0000000000ULL, 2}},
        {{std::nextafter(-1., -2.), -1}, {0x300000ffffffffffULL, 0xfffffffffffffffdULL}},
        {{std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()},
         {0xaaaaa9aaaaaaaaaaULL, 0xaaaaaaaaaaaaaaaaULL}}
    }};
    for (const auto &g : golden) EXPECT_EQ(mortonKey(g.point), g.key);
    EXPECT_EQ(mortonKey({-0., +0.}), mortonKey({+0., -0.}));
}

TEST_F(CacheStreamSort, Empty) { roundTrip({}, 1); }
TEST_F(CacheStreamSort, Single) { roundTrip({{-0., std::numeric_limits<double>::denorm_min(), -0.}}, 1); }
TEST_F(CacheStreamSort, OneBatchOneRun) { roundTrip(measurements(20), 20, 8192); }
TEST_F(CacheStreamSort, ManyBatchesOneRun) { roundTrip(measurements(20), 3, 8192); }
TEST_F(CacheStreamSort, FinalMergeOnly) { roundTrip(measurements(15), 4); }
TEST_F(CacheStreamSort, EmptyRunsAreSkippedAcrossGenerations)
{
    for (bool withPoints : {false, true}) {
        auto sorter = create(); ASSERT_TRUE(sorter);
        const auto data = measurements(withPoints ? Access::capacity(*sorter) * 2 : 0);
        ASSERT_TRUE(sorter->append(data));
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(Access::addEmptyRun(*sorter));
        std::vector<PointRecord> actual, expected;
        for (std::size_t i = 0; i < data.size(); ++i) expected.push_back({i, {data[i].x, data[i].y}});
        std::sort(expected.begin(), expected.end(), oracleLess);
        auto result = sorter->finish([&](auto block) {
            actual.insert(actual.end(), block.begin(), block.end()); return success();
        });
        ASSERT_TRUE(result); ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) EXPECT_EQ(actual[i].id, expected[i].id);
        EXPECT_GT(Access::stats(*sorter).mergeGenerations, 0);
        expectClean(*sorter);
    }
}
TEST_F(CacheStreamSort, ManyGenerationsPartialRun) { roundTrip(measurements(607), 17, 400, true); }
TEST_F(CacheStreamSort, AlreadySorted)
{
    auto input = measurements(241);
    std::stable_sort(input.begin(), input.end(), [](auto a, auto b) { return oracleLess({0, {a.x, a.y}}, {0, {b.x, b.y}}); });
    roundTrip(std::move(input), 13, 400, true);
}
TEST_F(CacheStreamSort, ReverseSorted)
{
    auto input = measurements(241);
    std::stable_sort(input.begin(), input.end(), [](auto a, auto b) { return oracleLess({0, {b.x, b.y}}, {0, {a.x, a.y}}); });
    roundTrip(std::move(input), 11, 400, true);
}
TEST_F(CacheStreamSort, CoincidentKeysKeepIdAndCoordinateBits)
{
    std::vector<Measurement> input;
    for (int i = 0; i < 220; ++i) input.push_back({i % 2 ? -0. : +0., i % 3 ? +0. : -0., -0.});
    roundTrip(std::move(input), 2, 400, true);
}
TEST_F(CacheStreamSort, DifferentBufferBoundaries)
{
    // blockRecords > 1 exercises partial input/output buffers and shared scratch reuse.
    auto sorter = create(8192); ASSERT_TRUE(sorter);
    const auto n = Access::capacity(*sorter) * (Access::fan(*sorter) + 1) + 1;
    sorter.reset();
    roundTrip(measurements(n), 73, 8192);
}
TEST_F(CacheStreamSort, BatchPartitionDoesNotAffectOrdering)
{
    auto data = measurements(180);
    roundTrip(data, 1); roundTrip(data, 180); roundTrip(data, 23);
}
TEST_F(CacheStreamSort, RandomFiniteBitPatterns)
{
    std::mt19937_64 random(9324297);
    auto coordinate = [&]() {
        const auto value = std::bit_cast<double>(random());
        return std::isfinite(value) ? value : 0.;
    };
    std::vector<Measurement> input;
    for (int i = 0; i < 1600; ++i) input.push_back({coordinate(), coordinate(), 0});
    roundTrip(std::move(input), 39, 400, true);
}
TEST_F(CacheStreamSort, MetadataAndSignedZeroBounds)
{
    auto sorter = create(); ASSERT_TRUE(sorter);
    const std::array data{Measurement{-0., +0., -0.}, Measurement{+0., -0., 0}};
    ASSERT_TRUE(sorter->append(data));
    auto result = sorter->finish([](auto) { return success(); }); ASSERT_TRUE(result);
    const auto &m = result.value();
    EXPECT_EQ(m.pointCount, 2); EXPECT_FALSE(m.hasNonZeroZ);
    EXPECT_EQ(bits(m.bounds.minX), bits(-0.)); EXPECT_EQ(bits(m.bounds.minY), bits(-0.));
    EXPECT_EQ(bits(m.bounds.maxX), bits(+0.)); EXPECT_EQ(bits(m.bounds.maxY), bits(+0.));
    auto other = create(); ASSERT_TRUE(other);
    const std::array extremes{Measurement{-1e300, 4, 0}, Measurement{2, -3, 1e-300}, Measurement{0, 0, 0}};
    ASSERT_TRUE(other->append(extremes));
    auto metadata = other->finish([](auto) { return success(); }); ASSERT_TRUE(metadata);
    EXPECT_NE(metadata.value().id, m.id); EXPECT_TRUE(metadata.value().hasNonZeroZ);
    EXPECT_EQ(metadata.value().bounds.minX, -1e300); EXPECT_EQ(metadata.value().bounds.maxX, 2);
    EXPECT_EQ(metadata.value().bounds.minY, -3); EXPECT_EQ(metadata.value().bounds.maxY, 4);
    auto empty = create(); ASSERT_TRUE(empty);
    auto em = empty->finish([](auto) { ADD_FAILURE(); return success(); }); ASSERT_TRUE(em);
    EXPECT_EQ(bits(em.value().bounds.minX), 0); EXPECT_EQ(bits(em.value().bounds.maxX), 0);
    EXPECT_EQ(bits(em.value().bounds.minY), 0); EXPECT_EQ(bits(em.value().bounds.maxY), 0);
    EXPECT_FALSE(em.value().hasNonZeroZ);
}
TEST_F(CacheStreamSort, NonFiniteInputIsTerminalAfterPrefix)
{
    for (auto v : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()}) {
        for (int axis = 0; axis < 3; ++axis) {
            auto sorter = create(); ASSERT_TRUE(sorter);
            ASSERT_TRUE(sorter->append(measurements(30)));
            Measurement invalid{1, 2, 3};
            if (axis == 0) invalid.x = v;
            if (axis == 1) invalid.y = v;
            if (axis == 2) invalid.z = v;
            const std::array data{Measurement{1, 1, 0}, invalid};
            auto result = sorter->append(data); ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::InvalidInput);
            EXPECT_EQ(sorter->append({}).error().code, result.error().code);
            auto finished = sorter->finish([](auto) { ADD_FAILURE(); return success(); });
            ASSERT_FALSE(finished); EXPECT_EQ(finished.error().code, result.error().code);
            expectClean(*sorter);
        }
    }
}
TEST_F(CacheStreamSort, CountOverflowBeforeAcceptance)
{
    auto sorter = create(); ASSERT_TRUE(sorter);
    Access::setCount(*sorter, UINT64_MAX - 1);
    auto status = sorter->append(measurements(2)); ASSERT_FALSE(status);
    EXPECT_EQ(status.error().code, ErrorCode::ResourceExhausted); expectClean(*sorter);
}
TEST_F(CacheStreamSort, InvalidAndInsufficientLimits)
{
    auto original = limits();
    for (int i = 0; i < 5; ++i) {
        auto l = original;
        if (i == 0) l.memoryBytes = 0;
        if (i == 1) l.temporaryDiskBytes = 0;
        if (i == 2) l.maxOpenFiles = 0;
        if (i == 3) l.memoryBytes = 100;
        if (i == 4) l.memoryBytes = UINT64_MAX;
        auto result = PointStreamSort::create(l, parent.path()); ASSERT_FALSE(result);
    }
    EXPECT_FALSE(PointStreamSort::create(original, parent.filePath(QStringLiteral("missing"))));
    EXPECT_TRUE(QDir(parent.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}
TEST_F(CacheStreamSort, DiskBudgetIncludesBothGenerations)
{
    auto l = limits(); l.temporaryDiskBytes = 15000;
    auto made = PointStreamSort::create(l, parent.path()); ASSERT_TRUE(made);
    auto sorter = std::move(made.value());
    ASSERT_TRUE(sorter->append(measurements(200)));
    auto result = sorter->finish([](auto) { ADD_FAILURE(); return success(); }); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(Access::stats(*sorter).mergeGenerations, 0);
    expectClean(*sorter);
}
TEST_F(CacheStreamSort, DiskBudgetDuringRunFormation)
{
    auto l = limits(); l.temporaryDiskBytes = 100;
    auto made = PointStreamSort::create(l, parent.path()); ASSERT_TRUE(made);
    auto sorter = std::move(made.value());
    auto result = sorter->append(measurements(100)); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted); expectClean(*sorter);
}
TEST_F(CacheStreamSort, OneHandleAllowsFinalMergeButRejectsIntermediate)
{
    auto l = limits(); l.maxOpenFiles = 1;
    for (auto n : {15, 200}) {
        auto made = PointStreamSort::create(l, parent.path()); ASSERT_TRUE(made);
        auto sorter = std::move(made.value()); ASSERT_TRUE(sorter->append(measurements(n)));
        auto result = sorter->finish([](auto) { return success(); });
        EXPECT_EQ(static_cast<bool>(result), n == 15);
        if (!result) EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
        EXPECT_EQ(Access::stats(*sorter).peakOpenFiles, 1); expectClean(*sorter);
    }
}

struct Cancellation {
    PointStreamSort *sorter = nullptr;
    Phase target = Phase::Ingest;
    int skip = 0;
    bool armed = false;
    static bool check(void *context) noexcept
    {
        auto &c = *static_cast<Cancellation *>(context);
        return c.armed && (!c.sorter || Access::phase(*c.sorter) == c.target) && c.skip-- <= 0;
    }
};
TEST_F(CacheStreamSort, CancellationCheckpointsAndCleanup)
{
    for (const auto phase : {Phase::Ingest, Phase::RunSort, Phase::RunWrite, Phase::Merge, Phase::Output}) {
        for (int skip : {0, 1, 7}) {
            Cancellation cancel; cancel.target = phase; cancel.skip = skip;
            auto sorter = create(400, Cancellation::check, &cancel); ASSERT_TRUE(sorter);
            cancel.sorter = sorter.get();
            if (phase == Phase::Ingest || phase == Phase::RunSort || phase == Phase::RunWrite) cancel.armed = true;
            auto appended = sorter->append(measurements(600));
            if (appended) { cancel.armed = true; auto result = sorter->finish([](auto) { return success(); });
                ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
            } else EXPECT_EQ(appended.error().code, ErrorCode::Cancelled);
            EXPECT_EQ(sorter->append({}).error().code, ErrorCode::Cancelled); expectClean(*sorter);
        }
    }
    Cancellation cancel; cancel.armed = true;
    EXPECT_FALSE(PointStreamSort::create(limits(), parent.path(), Cancellation::check, &cancel));
}
TEST_F(CacheStreamSort, CancelAtFinalSuccessCheckpoint)
{
    Cancellation cancel;
    auto sorter = create(400, Cancellation::check, &cancel); ASSERT_TRUE(sorter);
    cancel.sorter = sorter.get(); cancel.target = Phase::Output;
    ASSERT_TRUE(sorter->append(measurements(1)));
    auto result = sorter->finish([&](auto) { cancel.armed = true; return success(); });
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::Cancelled); expectClean(*sorter);
}
TEST_F(CacheStreamSort, CancelBetweenGenerations)
{
    PointStreamSort *operation = nullptr;
    auto sorter = create(400, [](void *context) noexcept {
        const auto s = *static_cast<PointStreamSort **>(context);
        return s && Access::stats(*s).mergeGenerations == 1;
    }, &operation);
    ASSERT_TRUE(sorter); operation = sorter.get();
    ASSERT_TRUE(sorter->append(measurements(600)));
    auto result = sorter->finish([](auto) { ADD_FAILURE(); return success(); }); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::Cancelled);
    EXPECT_EQ(Access::stats(*sorter).mergeGenerations, 1); expectClean(*sorter);
}
TEST_F(CacheStreamSort, ConsumerFailureAndExceptionAreTerminal)
{
    for (bool throwing : {false, true}) {
        auto sorter = create(); ASSERT_TRUE(sorter); ASSERT_TRUE(sorter->append(measurements(300)));
        int calls = 0;
        auto result = sorter->finish([&](auto) -> Status {
            if (++calls == 3) {
                if (throwing) throw std::runtime_error("test sink");
                return makeError(ErrorCode::ConsumerFailed, Phase::Output, Operation::Consume, QStringLiteral("test sink"));
            }
            return success();
        });
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::ConsumerFailed);
        EXPECT_EQ(calls, 3); expectClean(*sorter);
        EXPECT_EQ(sorter->finish([](auto) { ADD_FAILURE(); return success(); }).error().code, ErrorCode::ConsumerFailed);
    }
}
TEST_F(CacheStreamSort, MissingSinkAndReentry)
{
    auto invalid = create(); ASSERT_TRUE(invalid);
    EXPECT_FALSE(invalid->finish({})); expectClean(*invalid);
    auto sorter = create(); ASSERT_TRUE(sorter); ASSERT_TRUE(sorter->append(measurements(2)));
    auto result = sorter->finish([&](auto) {
        EXPECT_EQ(sorter->append({}).error().code, ErrorCode::InvalidState);
        EXPECT_EQ(sorter->finish([](auto) { return success(); }).error().code, ErrorCode::InvalidState);
        return success();
    });
    EXPECT_TRUE(result); expectClean(*sorter);
}

// Hooks use real QFile devices and the existing exact-transfer primitives. No
// alternate filesystem or alternate merge implementation is involved.
struct Fault {
    inline static Phase target;
    inline static Operation operation;
    inline static int skip = 0;
    static Status inject(PointStreamSort &s, Operation op, QFile &file)
    {
        if (Access::phase(s) != target || op != operation || skip-- > 0) return success();
        const auto code = op == Operation::Open ? ErrorCode::OpenFailed
            : op == Operation::Write ? ErrorCode::WriteFailed : op == Operation::Read ? ErrorCode::ReadFailed
            : op == Operation::Seek ? ErrorCode::SeekFailed : ErrorCode::FlushFailed;
        return makeError(code, target, op, QStringLiteral("injected I/O failure"),
                         file.pos() < 0 ? std::nullopt : std::optional{static_cast<std::uint64_t>(file.pos())}, 3);
    }
};
TEST_F(CacheStreamSort, FileFaultsDuringRunMergeAndOutput)
{
    struct Case { Phase phase; Operation op; int skip; };
    const std::array cases{
        Case{Phase::RunWrite, Operation::Open, 0}, Case{Phase::RunWrite, Operation::Write, 2},
        Case{Phase::RunWrite, Operation::Seek, 0}, Case{Phase::RunWrite, Operation::Flush, 0},
        Case{Phase::Merge, Operation::Open, 0}, Case{Phase::Merge, Operation::Read, 4},
        Case{Phase::Merge, Operation::Write, 4}, Case{Phase::Merge, Operation::Seek, 4},
        Case{Phase::Merge, Operation::Flush, 0}, Case{Phase::Output, Operation::Read, 4}};
    for (const auto &c : cases) {
        auto sorter = create(); ASSERT_TRUE(sorter);
        Fault::target = c.phase; Fault::operation = c.op; Fault::skip = c.skip;
        Access::hook(*sorter, Fault::inject);
        auto appended = sorter->append(measurements(607));
        if (appended) {
            auto result = sorter->finish([](auto) { return success(); });
            ASSERT_FALSE(result); EXPECT_EQ(result.error().detail, QStringLiteral("injected I/O failure"));
        } else EXPECT_EQ(appended.error().detail, QStringLiteral("injected I/O failure"));
        expectClean(*sorter);
    }
}
TEST_F(CacheStreamSort, RealOpenFailure)
{
    auto sorter = create(); ASSERT_TRUE(sorter);
    Access::hook(*sorter, [](PointStreamSort &, Operation op, QFile &file) -> Status {
        if (op == Operation::Open)
            file.setFileName(file.fileName() + QStringLiteral("/missing/run"));
        return success();
    });
    auto result = sorter->append(measurements(40)); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::OpenFailed); expectClean(*sorter);
}
TEST_F(CacheStreamSort, AllocationExceptionsBecomeStructuredTerminalErrors)
{
    for (const auto phase : {Phase::RunWrite, Phase::Merge}) {
        auto sorter = create(); ASSERT_TRUE(sorter);
        Fault::target = phase;
        Access::hook(*sorter, [](PointStreamSort &s, Operation op, QFile &) -> Status {
            if (Access::phase(s) == Fault::target && op == Operation::Open) throw std::bad_alloc{};
            return success();
        });
        auto appended = sorter->append(measurements(600));
        if (appended) {
            auto result = sorter->finish([](auto) { return success(); }); ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
        } else EXPECT_EQ(appended.error().code, ErrorCode::ResourceExhausted);
        EXPECT_EQ(sorter->append({}).error().code, ErrorCode::ResourceExhausted);
        expectClean(*sorter);
    }
}
TEST_F(CacheStreamSort, TruncationDuringReadReachesExactTransferEOF)
{
    for (const auto phase : {Phase::Merge, Phase::Output}) {
        auto sorter = create(); ASSERT_TRUE(sorter); ASSERT_TRUE(sorter->append(measurements(600)));
        Fault::target = phase;
        Access::hook(*sorter, [](PointStreamSort &s, Operation op, QFile &file) -> Status {
            if (Access::phase(s) == Fault::target && op == Operation::Read && file.pos() == 32)
                EXPECT_TRUE(file.resize(file.pos()));
            return success();
        });
        auto result = sorter->finish([](auto) { return success(); }); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::IncompleteCache); expectClean(*sorter);
    }
}
TEST_F(CacheStreamSort, DetectsCorruptAndTruncatedRun)
{
    for (int mode = 0; mode < 5; ++mode) {
        auto sorter = create(); ASSERT_TRUE(sorter);
        // Exactly full runs, so finish will not append after the injected damage.
        ASSERT_TRUE(sorter->append(measurements(Access::capacity(*sorter) * 7)));
        auto &file = *Access::source(*sorter);
        if (mode == 0) ASSERT_TRUE(file.resize(file.size() - 1));
        else {
            const auto offset = mode == 1 ? 0 : mode == 2 ? 8 : mode == 3 ? 24 : 32 + 16 + 8;
            ASSERT_TRUE(file.seek(offset)); auto data = file.read(1); ASSERT_EQ(data.size(), 1);
            data[0] = static_cast<char>(data[0] ^ 1);
            ASSERT_TRUE(file.seek(offset)); ASSERT_EQ(file.write(data), 1);
        }
        auto result = sorter->finish([](auto) { return success(); }); ASSERT_FALSE(result);
        EXPECT_TRUE(result.error().code == ErrorCode::CorruptCache || result.error().code == ErrorCode::IncompleteCache);
        expectClean(*sorter);
    }
}
TEST_F(CacheStreamSort, NewGenerationVerifiedBeforeOldRemoval)
{
    auto sorter = create(); ASSERT_TRUE(sorter); ASSERT_TRUE(sorter->append(measurements(600)));
    Access::hook(*sorter, [](PointStreamSort &s, Operation op, QFile &f) -> Status {
        if (Access::phase(s) == Phase::Merge && op == Operation::Flush) {
            EXPECT_NE(Access::source(s), &f);
            EXPECT_TRUE(QFile::exists(Access::source(s)->fileName()));
            EXPECT_EQ(QDir(Access::path(s)).entryList(QDir::Files).size(), 2);
            if (Access::stats(s).mergeGenerations == 1) {
                EXPECT_TRUE(f.resize(f.size() - 1));
            }
        }
        return success();
    });
    auto result = sorter->finish([](auto) { ADD_FAILURE(); return success(); }); ASSERT_FALSE(result);
    EXPECT_EQ(Access::stats(*sorter).mergeGenerations, 1); expectClean(*sorter);
}
TEST_F(CacheStreamSort, ResourcesDoNotGrowWithPointCountAndOperationsAreIsolated)
{
    auto first = create(), second = create(); ASSERT_TRUE(first); ASSERT_TRUE(second);
    EXPECT_NE(Access::path(*first), Access::path(*second));
    ASSERT_TRUE(first->append(measurements(20))); ASSERT_TRUE(second->append(measurements(1000)));
    auto a = first->finish([](auto) { return success(); }); ASSERT_TRUE(a);
    EXPECT_TRUE(QDir(Access::path(*second)).exists());
    auto b = second->finish([](auto) { return success(); }); ASSERT_TRUE(b);
    EXPECT_EQ(Access::stats(*first).peakBufferBytes, Access::stats(*second).peakBufferBytes);
    EXPECT_LE(Access::stats(*second).peakOpenFiles, 2);
    expectClean(*second);
    auto abandoned = create(); ASSERT_TRUE(abandoned); ASSERT_TRUE(abandoned->append(measurements(30)));
    abandoned.reset();
    EXPECT_TRUE(QDir(parent.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
}
TEST_F(CacheStreamSort, NativeHandleCountAndContainerCountStayBounded)
{
    // Warm up Qt UUID/temp-path infrastructure before observing native handles.
    { auto warmup = create(); ASSERT_TRUE(warmup); }
    static DWORD baseline = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &baseline));
    auto sorter = create(); ASSERT_TRUE(sorter);
    Access::hook(*sorter, [](PointStreamSort &s, Operation op, QFile &) -> Status {
        if (op != Operation::Open && op != Operation::Flush) return success();
        DWORD current = 0;
        EXPECT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &current));
        EXPECT_LE(current, baseline + 2);
        // QFile engines are unbuffered and destroyed before removal; no closed
        // QTemporaryFile object may retain a third native file handle here.
        const auto files = QDir(Access::path(s)).entryInfoList(QDir::Files);
        EXPECT_LE(files.size(), 2);
        std::uint64_t bytes = 0;
        for (const auto &f : files) bytes += static_cast<std::uint64_t>(f.size());
        EXPECT_LE(bytes, Access::stats(s).peakPlannedDiskBytes);
        for (auto f : {Access::source(s), Access::destination(s)})
            if (f && f->isOpen()) EXPECT_NE(f->handle(), -1);
        return success();
    });
    ASSERT_TRUE(sorter->append(measurements(900)));
    DWORD during = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &during));
    EXPECT_LE(during, baseline + 1);
    auto result = sorter->finish([&](auto) {
        DWORD current = 0;
        EXPECT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &current));
        EXPECT_LE(current, baseline + 1);
        return success();
    });
    ASSERT_TRUE(result); expectClean(*sorter);
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &during));
    EXPECT_EQ(during, baseline);
}
TEST_F(CacheStreamSort, CleanupFailurePreservesPrimaryErrorAndDestructorRetries)
{
    for (bool sinkFails : {false, true}) {
        auto sorter = create(); ASSERT_TRUE(sorter); ASSERT_TRUE(sorter->append(measurements(3)));
        HANDLE heldFile = INVALID_HANDLE_VALUE;
        auto result = sorter->finish([&](auto) -> Status {
            if (heldFile == INVALID_HANDLE_VALUE) {
                const auto path = Access::source(*sorter)->fileName();
                // Deliberately deny deletion until the operation has returned.
                heldFile = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                EXPECT_NE(heldFile, INVALID_HANDLE_VALUE);
            }
            if (sinkFails)
                return makeError(ErrorCode::ConsumerFailed, Phase::Output, Operation::Consume, QStringLiteral("primary sink error"));
            return success();
        });
        EXPECT_FALSE(result);
        if (!result) EXPECT_EQ(result.error().code, sinkFails ? ErrorCode::ConsumerFailed : ErrorCode::CleanupFailed);
        EXPECT_TRUE(QDir(Access::path(*sorter)).exists());
        EXPECT_EQ(Access::stats(*sorter).openFiles, 0);
        if (heldFile != INVALID_HANDLE_VALUE) EXPECT_TRUE(CloseHandle(heldFile));
        sorter.reset();
        EXPECT_TRUE(QDir(parent.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
}
} // namespace
