#include "density/density_builder.h"
#include "cache/cache_writer.h"

#include <gtest/gtest.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#define NOMINMAX
#include <Windows.h>

namespace cadcontour::density {
class DensityBuilderTestAccess {
public:
    using Point = DensityBuilder::FaultPoint;
    using Hook = DensityBuilder::FaultHook;
    static BuildResult build(const BuildRequest &r, Hook hook, void *context) {
        return DensityBuilder::buildWithHook(r, hook, context);
    }
    using Transfer = DensityBuilder::TransferHook;
    static BuildResult transfer(const BuildRequest &r, Transfer hook, void *context) {
        return DensityBuilder::buildWithTransferHook(r, nullptr, nullptr, hook, context);
    }
};
}
namespace {
using namespace cadcontour;
using namespace cadcontour::density;
using Pair = std::pair<U64, U64>;
using Counts = std::map<Pair, U64>;
constexpr cache::ResourceLimits cacheLimits{4 * 1024 * 1024, 128 * 1024 * 1024, 4};

// Independent small-fixture oracle: enumerate intervals, never call Mapping or
// production addressing. Volatile separates the two RN binary64 operations.
std::vector<double> boundaries(double minimum, double maximum, double cell)
{
    std::vector<double> out{minimum};
    do {
        volatile double product = static_cast<double>(out.size()) * cell;
        volatile double next = minimum + product;
        if (!(next > out.back()) || out.size() > 2'000'000)
            throw std::runtime_error("invalid oracle fixture");
        out.push_back(static_cast<double>(next));
    } while (out.back() < maximum);
    return out;
}
U64 interval(const std::vector<double> &edges, double originalMaximum, double point)
{
    for (std::size_t i = 0; i + 1 < edges.size(); ++i)
        if (point >= edges[i] && (point < edges[i + 1]
            || (i + 2 == edges.size() && point == originalMaximum))) return i;
    throw std::runtime_error("oracle point outside intervals");
}
struct Expected {
    U64 width, height;
    Counts cells;
};
Expected oracle(std::span<const cache::Measurement> input, const ImportedDataset &source, double cell)
{
    auto x = boundaries(source.minimum.x, source.maximum.x, cell);
    auto y = boundaries(source.minimum.y, source.maximum.y, cell);
    Expected result{x.size() - 1, y.size() - 1, {}};
    std::map<double, U64> xi, yi;
    for (auto p : input) {
        if (!xi.contains(p.x)) xi[p.x] = interval(x, source.maximum.x, p.x);
        if (!yi.contains(p.y)) yi[p.y] = interval(y, source.maximum.y, p.y);
        ++result.cells[{xi[p.x], yi[p.y]}];
    }
    return result;
}
QByteArray digest(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("fixture read");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) throw std::runtime_error("fixture hash");
    return hash.result();
}
struct Fixture {
    QTemporaryDir dir;
    std::optional<cache::PendingCache> cache;
    ImportedDataset source;
    std::vector<cache::Measurement> input;
    explicit Fixture(std::vector<cache::Measurement> points) : input(std::move(points))
    { initialize(); }
    Fixture(std::vector<cache::Measurement> points, const QString &directoryTemplate)
        : dir(directoryTemplate), input(std::move(points))
    { initialize(); }
    void initialize()
    {
        if (!dir.isValid()) throw std::runtime_error("fixture directory");
        auto writer = cache::CacheWriter::create(dir.filePath("source.pc2dcache"), cacheLimits, dir.path());
        if (!writer || !writer.value()->append(input)) throw std::runtime_error("fixture cache write");
        auto completed = writer.value()->finish();
        if (!completed) throw std::runtime_error("fixture cache finish");
        cache.emplace(std::move(completed.value()));
        const auto &metadata = cache->metadata();
        source.cacheId = metadata.id; source.pointCount = metadata.pointCount;
        source.minimum = {metadata.bounds.minX, metadata.bounds.minY};
        source.maximum = {metadata.bounds.maxX, metadata.bounds.maxY};
        source.location = std::make_shared<const DatasetLocation>(DatasetLocation{
            "density-builder-fixture", std::filesystem::path(dir.path().toStdWString()), L"source.pc2dcache"});
    }
    BuildRequest request(double cell = 1, unsigned side = 256, U64 capacity = 31) const
    {
        BuildRequest r;
        r.source = source; r.cell = cell; r.tileSide = side; r.eventCapacity = capacity;
        r.outputPath = dir.filePath("result.density"); r.scratchParent = dir.path();
        r.resources = initialBuildResourcePolicy();
        return r;
    }
    void clean() const
    {
        EXPECT_FALSE(QFileInfo::exists(dir.filePath("result.density")));
        EXPECT_EQ(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
            QStringList{"source.pc2dcache"});
    }
};

void verify(const BuildOutput &output, const Expected &expected, U64 measurements, unsigned side)
{
    auto policy = initialResourcePolicy();
    auto opened = Reader::openReader(output.dataset.path(), policy);
    ASSERT_TRUE(opened) << (opened ? "" : opened.error().detail.toStdString());
    auto &reader = *opened.value();
    EXPECT_FALSE(reader.metadata().id.isNull());
    EXPECT_EQ(reader.metadata().contract, BuildContract::CompletePointCache);
    EXPECT_EQ(reader.metadata().sourceMeasurements, measurements);
    EXPECT_EQ(reader.metadata().tileSide, side);
    EXPECT_EQ(reader.mapping().width(), expected.width);
    EXPECT_EQ(reader.mapping().height(), expected.height);
    std::set<Pair> keys;
    for (auto [cell, count] : expected.cells) { (void)count; keys.emplace(cell.first / side, cell.second / side); }
    EXPECT_EQ(reader.statistics().physicalTiles, keys.size());
    U64 total = 0;
    for (auto key : keys) {
        auto tile = reader.readTile({key.first, key.second}); ASSERT_TRUE(tile);
        const auto width = std::min<U64>(side, expected.width - key.first * side);
        const auto height = std::min<U64>(side, expected.height - key.second * side);
        ASSERT_EQ(tile.value().shape.width, width); ASSERT_EQ(tile.value().shape.height, height);
        ASSERT_EQ(tile.value().counts.size(), width * height);
        for (U64 y = 0; y < height; ++y) for (U64 x = 0; x < width; ++x) {
            auto found = expected.cells.find({key.first * side + x, key.second * side + y});
            const auto count = found == expected.cells.end() ? 0 : found->second;
            ASSERT_EQ(tile.value().counts[static_cast<std::size_t>(y * width + x)], count)
                << "tile=" << key.first << ',' << key.second << " local=" << x << ',' << y;
            total += count;
        }
    }
    EXPECT_EQ(total, measurements);
    // Probe an omitted tile without enumerating the logical area.
    for (U64 y = 0; y < std::min<U64>(3, (expected.height - 1) / side + 1); ++y)
        for (U64 x = 0; x < std::min<U64>(3, (expected.width - 1) / side + 1); ++x)
            if (!keys.contains({x, y})) {
                auto zero = reader.readTile({x, y}); ASSERT_TRUE(zero);
                EXPECT_TRUE(std::all_of(zero.value().counts.begin(), zero.value().counts.end(), [](U64 v) { return !v; }));
            }
}

TEST(EndToEnd, SinglePointAndDiscard)
{
    Fixture f({{2.5, -3, 0}});
    {
        auto result = DensityBuilder::build(f.request()); ASSERT_TRUE(result);
        verify(result.value(), oracle(f.input, f.source, 1), 1, 256);
    }
    f.clean();
}

void invariants(const BuildStatistics &s, U64 n)
{
    EXPECT_EQ(s.scanAllCalls, 1); EXPECT_EQ(s.spatialQueries, 0);
    EXPECT_EQ(s.scanned, n); EXPECT_EQ(s.eventsWritten, n); EXPECT_EQ(s.eventsRead, n);
    EXPECT_EQ(s.accumulated, n); EXPECT_EQ(s.tileTotal, n); EXPECT_EQ(s.density.total, n);
    EXPECT_EQ(s.eventBytesWritten, n * 4); EXPECT_EQ(s.eventBytesRead, n * 4);
    EXPECT_EQ(s.visitedGroups, s.occupiedGroups);
}
class Spatial : public testing::TestWithParam<unsigned> {};
TEST_P(Spatial, PartialEdgesGroupBoundaryAndRepeatedChunks)
{
    std::vector<cache::Measurement> points{{-600, -600, 0}, {431.25, 435.5, 0}};
    for (int i = 0; i < 119; ++i) {
        points.push_back({-89, -89, 0}); points.push_back({-88, -88, 0});
        points.push_back({169, -344, 0}); points.push_back({-345, 171, 0});
    }
    Fixture f(std::move(points));
    auto expected = oracle(f.input, f.source, 1);
    for (auto capacity : {U64{1}, U64{17}, U64{1024}}) {
        {
            auto r = f.request(1, GetParam(), capacity);
            U64 progressCalls = 0; r.context = &progressCalls;
            r.progress = [](const BuildProgress &, void *context) noexcept { ++*static_cast<U64 *>(context); };
            auto result = DensityBuilder::build(r); ASSERT_TRUE(result) << result.error().detail.toStdString();
            verify(result.value(), expected, f.input.size(), GetParam());
            invariants(result.value().statistics, f.input.size());
            EXPECT_LT(progressCalls, 30); // capacity=1 still cannot emit per point
        }
        f.clean();
    }
}
INSTANTIATE_TEST_SUITE_P(TileSides, Spatial, testing::Values(256u, 512u));

TEST(EndToEnd, MoreThan65535DuplicatesInOneCell)
{
    Fixture f(std::vector<cache::Measurement>(65539, {-0.0, +0.0, 0}));
    auto result = DensityBuilder::build(f.request()); ASSERT_TRUE(result);
    verify(result.value(), {1, 1, {{{0, 0}, 65539}}}, f.input.size(), 256);
    invariants(result.value().statistics, f.input.size());
}
TEST(EndToEnd, DegenerateXAndY)
{
    for (bool vertical : {false, true}) {
        Fixture f(vertical ? std::vector<cache::Measurement>{{-0., -3., 0}, {+0., 520.25, 0}, {0, 509, 0}}
            : std::vector<cache::Measurement>{{-3., -0., 0}, {520.25, +0., 0}, {509, 0, 0}});
        auto result = DensityBuilder::build(f.request()); ASSERT_TRUE(result);
        verify(result.value(), oracle(f.input, f.source, 1), f.input.size(), 256);
    }
}

struct Golden { const char *name; U64 minimum, cell, maximum; std::vector<U64> boundaries; };
const Golden goldens[]{
#include "fixtures/raster/raster_boundaries.inc"
};
class Numerical : public testing::TestWithParam<std::size_t> {};
TEST_P(Numerical, RationalGoldenBoundariesAndNeighbours)
{
    const auto &g = goldens[GetParam()]; SCOPED_TRACE(g.name);
    const double minimum = std::bit_cast<double>(g.minimum), maximum = std::bit_cast<double>(g.maximum);
    const double cell = std::bit_cast<double>(g.cell);
    std::vector<cache::Measurement> points;
    Counts expected;
    auto add = [&](double x, U64 index) { points.push_back({x, -0., 0}); ++expected[{index, 0}]; };
    add(minimum, 0); add(maximum, g.boundaries.size() - 2);
    for (std::size_t i = 1; i + 1 < g.boundaries.size(); ++i) {
        const auto boundary = std::bit_cast<double>(g.boundaries[i]);
        if (boundary <= minimum || boundary >= maximum) continue;
        add(boundary, i);
        add(std::nextafter(boundary, minimum), i - 1);
        const auto above = std::nextafter(boundary, maximum);
        // Very coarse/subnormal fixtures may have no representable interior.
        U64 index = i;
        while (index + 1 < g.boundaries.size() - 1 && above >= std::bit_cast<double>(g.boundaries[index + 1])) ++index;
        add(above, above == maximum ? g.boundaries.size() - 2 : index);
    }
    Fixture f(std::move(points));
    auto result = DensityBuilder::build(f.request(cell)); ASSERT_TRUE(result) << result.error().detail.toStdString();
    verify(result.value(), {g.boundaries.size() - 1, 1, expected}, f.input.size(), 256);
}
INSTANTIATE_TEST_SUITE_P(Rational, Numerical, testing::Range(std::size_t{0}, std::size(goldens)));

TEST(EndToEnd, SparseAreaGrowthPreservesOccupiedWork)
{
    std::optional<BuildStatistics> previous;
    for (auto extent : {8192., 131072.}) {
        Fixture f({{0, 0, 0}, {extent, extent, 0}, {0, 0, 0}, {extent - 1, extent - 1, 0}});
        auto result = DensityBuilder::build(f.request()); ASSERT_TRUE(result);
        verify(result.value(), oracle(f.input, f.source, 1), f.input.size(), 256);
        const auto s = result.value().statistics;
        invariants(s, f.input.size()); EXPECT_EQ(s.occupiedGroups, 2); EXPECT_EQ(s.tiles, 2);
        if (previous) {
            EXPECT_EQ(s.eventBytesWritten, previous->eventBytesWritten);
            EXPECT_EQ(s.eventOpens, previous->eventOpens); EXPECT_EQ(s.filesPeak, previous->filesPeak);
        }
        previous = s;
    }
}
TEST(EndToEnd, ConstructionProfileDoesNotChangeDistribution)
{
    Fixture f({{-10, -20, 0}, {1200.25, 1300.5, 0}, {246, 236, 0}, {502, 492, 0}, {502, 492, 0}});
    auto expected = oracle(f.input, f.source, 1);
    for (unsigned group : {256u, 512u, 1024u}) {
        auto r = f.request(); r.groupSide = group;
        auto result = DensityBuilder::build(r); ASSERT_TRUE(result);
        verify(result.value(), expected, f.input.size(), 256);
        invariants(result.value().statistics, f.input.size());
    }
}

TEST(Addressing, HugePairKeysAndCheckedLocalRange)
{
    const U64 dimension = U64{1} << 53;
    auto a = buildGroupAddress(dimension, dimension, 512, 256, {dimension - 1, dimension - 2});
    ASSERT_TRUE(a); EXPECT_EQ(a.value().key.x, (dimension - 1) / 512);
    EXPECT_EQ(a.value().key.y, (dimension - 2) / 512);
    EXPECT_EQ(a.value().localIndex, 510u * 512 + 511);
    EXPECT_FALSE(buildGroupAddress(5, 5, 0, 256, {0, 0}));
    EXPECT_FALSE(buildGroupAddress(5, 5, 513, 256, {0, 0}));
    EXPECT_TRUE(buildGroupAddress(5, 5, 65536, 256, {0, 0}));
    EXPECT_FALSE(buildGroupAddress(5, 5, 65792, 256, {0, 0}));
    EXPECT_FALSE(buildGroupAddress(5, 5, 512, 256, {5, 0}));
    EXPECT_FALSE(cache::checked::add(std::numeric_limits<U64>::max(), 1));
    EXPECT_FALSE(cache::checked::multiply(std::numeric_limits<U64>::max(), 4));
    auto u32 = buildCheckedCountAdd(UINT32_MAX, 1); ASSERT_TRUE(u32); EXPECT_EQ(u32.value(), U64{UINT32_MAX} + 1);
    auto u64 = buildCheckedCountAdd(UINT64_MAX - 1, 1); ASSERT_TRUE(u64); EXPECT_EQ(u64.value(), UINT64_MAX);
    EXPECT_FALSE(buildCheckedCountAdd(UINT64_MAX, 1));
}

TEST(InvalidSource, NullMissingWrongBindingAndMetadataMismatch)
{
    Fixture f({{1, 2, 0}, {3, 4, 0}});
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        auto r = f.request();
        switch (mutation) {
        case 0: r.source.location.reset(); break;
        case 1: r.source.location = std::make_shared<const DatasetLocation>(DatasetLocation{}); break;
        case 2: {
            auto loc = *r.source.location; loc.relativeCachePath = L"missing";
            r.source.location = std::make_shared<const DatasetLocation>(std::move(loc)); break;
        }
        case 3: r.source.cacheId.bytes[0] ^= std::byte{1}; break;
        case 4: ++r.source.pointCount; break;
        case 5: r.source.minimum.x = 0; break;
        case 6: r.source.maximum.y = 5; break;
        case 7: r.source.cacheId = {}; break;
        }
        auto result = DensityBuilder::build(r); EXPECT_FALSE(result) << mutation;
        f.clean();
    }
}
TEST(InvalidSource, EmptyCache)
{
    Fixture f({}); auto result = DensityBuilder::build(f.request()); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, BuildErrorCode::EmptySource); f.clean();
}
TEST(InvalidSource, CorruptPointPageAndTruncation)
{
    for (bool truncate : {false, true}) {
        Fixture f({{0, 0, 0}, {400, 600, 0}});
        QFile file(f.cache->path()); ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        if (truncate) ASSERT_TRUE(file.resize(file.size() - 7));
        else { ASSERT_TRUE(file.seek(cache::format::headerSize + 65)); ASSERT_EQ(file.write("\xff", 1), 1); }
        file.close();
        auto result = DensityBuilder::build(f.request()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, BuildErrorCode::CacheFailure); f.clean();
    }
}

TEST(Lifecycle, ExistingTargetAndOwnershipTransfer)
{
    Fixture f({{0, 0, 0}, {10, 10, 0}});
    const auto original = digest(f.cache->path());
    auto r = f.request();
    { QFile target(r.outputPath); ASSERT_TRUE(target.open(QIODevice::WriteOnly)); ASSERT_EQ(target.write("sentinel"), 8); }
    const auto sentinel = digest(r.outputPath);
    EXPECT_FALSE(DensityBuilder::build(r)); EXPECT_EQ(digest(r.outputPath), sentinel);
    ASSERT_TRUE(QFile::remove(r.outputPath));
    QString accepted;
    {
        auto result = DensityBuilder::build(r); ASSERT_TRUE(result);
        accepted = result.value().dataset.releaseToOwner();
    }
    EXPECT_TRUE(QFileInfo::exists(accepted)); EXPECT_EQ(digest(f.cache->path()), original);
    r.outputPath = f.dir.filePath("second.density");
    { auto result = DensityBuilder::build(r); ASSERT_TRUE(result); }
    EXPECT_FALSE(QFileInfo::exists(r.outputPath)); EXPECT_TRUE(QFileInfo::exists(accepted));
    ASSERT_TRUE(QFile::remove(accepted)); f.clean();
}

class Budget : public testing::TestWithParam<unsigned> {};
TEST_P(Budget, ZeroRefusesWithoutArtifacts)
{
    Fixture f({{0, 0, 0}, {800, 800, 0}}); auto r = f.request();
    switch (GetParam()) {
    case 0: r.resources.workingBytes = 0; break;
    case 1: r.resources.metadataBytes = 0; break;
    case 2: r.resources.readerBytes = 0; break;
    case 3: r.resources.scratchBytes = 0; break;
    case 4: r.resources.outputBytes = 0; break;
    case 5: r.resources.combinedDiskBytes = 0; break;
    case 6: r.resources.files = 0; break;
    case 7: r.resources.handles = 0; break;
    case 8: r.resources.workUnits = 0; break;
    case 9: r.resources.mapping = raster::ValidationPolicy{0}; break;
    case 10: r.resources.densityBufferBytes = 0; break;
    case 11: r.resources.densityCodecBytes = 0; break;
    case 12: r.resources.densitySortBytes = 0; break;
    }
    auto result = DensityBuilder::build(r); ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().code == BuildErrorCode::ResourceRefused
        || result.error().code == BuildErrorCode::DensityFailure
        || result.error().code == BuildErrorCode::Mapping
        || result.error().code == BuildErrorCode::InvalidInput);
    f.clean();
}
INSTANTIATE_TEST_SUITE_P(ResourceCeilings, Budget, testing::Range(0u, 13u));

TEST(Resources, ExactWorkingAndMetadataReservationAndOneByteLess)
{
    Fixture f({{0, 0, 0}, {700, 700, 0}, {1200, 50, 0}});
    BuildStatistics stats;
    { auto baseline = DensityBuilder::build(f.request()); ASSERT_TRUE(baseline); stats = baseline.value().statistics; }
    for (bool memory : {false, true}) {
        auto r = f.request();
        auto &ceiling = memory ? r.resources.workingBytes : r.resources.metadataBytes;
        ceiling = memory ? stats.workingPeak : stats.metadataPeak;
        ASSERT_GT(ceiling, 0);
        { auto exact = DensityBuilder::build(r); ASSERT_TRUE(exact) << exact.error().detail.toStdString(); }
        --ceiling;
        auto refused = DensityBuilder::build(r); ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, BuildErrorCode::ResourceRefused); f.clean();
    }
}
TEST(Resources, CombinedDiskAndJointRamRefuseDespiteIndividualReservations)
{
    Fixture f({{0, 0, 0}, {700, 700, 0}}); BuildStatistics s;
    { auto baseline = DensityBuilder::build(f.request()); ASSERT_TRUE(baseline); s = baseline.value().statistics; }
    auto r = f.request(); r.resources.combinedDiskBytes = s.combinedDiskPeak - 1;
    { auto refused = DensityBuilder::build(r); ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code, BuildErrorCode::ResourceRefused); }
    r = f.request();
    // Each component fits this RAM ceiling individually; their phase sum does not.
    r.resources.workingBytes = std::max({r.resources.readerBytes, r.resources.densityBufferBytes,
        r.resources.densityCodecBytes, r.resources.densitySortBytes, U64{512 * 512 * 8}});
    EXPECT_FALSE(DensityBuilder::build(r)); f.clean();
}
class ExactBudget : public testing::TestWithParam<unsigned> {};
TEST_P(ExactBudget, ExactCeilingAndOneUnitLess)
{
    Fixture f({{0, 0, 0}, {700, 700, 0}, {1200, 1200, 0}}); BuildStatistics stats;
    { auto baseline = DensityBuilder::build(f.request()); ASSERT_TRUE(baseline); stats = baseline.value().statistics; }
    auto r = f.request();
    switch (GetParam()) {
    case 0: r.resources.scratchBytes = stats.scratchPeak; break;
    case 1: r.resources.outputBytes = stats.density.outputBytes; break;
    case 2: r.resources.combinedDiskBytes = stats.combinedDiskPeak; break;
    case 3: r.resources.files = stats.filesPeak; break;
    case 4: r.resources.handles = static_cast<std::uint32_t>(stats.handlesPeak); break;
    case 5: r.resources.workUnits = stats.workUnits; break;
    }
    { auto exact = DensityBuilder::build(r); ASSERT_TRUE(exact) << exact.error().detail.toStdString(); }
    switch (GetParam()) {
    case 0: --r.resources.scratchBytes; break;
    case 1: --r.resources.outputBytes; break;
    case 2: --r.resources.combinedDiskBytes; break;
    case 3: --r.resources.files; break;
    case 4: --r.resources.handles; break;
    case 5: --r.resources.workUnits; break;
    }
    auto refused = DensityBuilder::build(r); ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, BuildErrorCode::ResourceRefused); f.clean();
}
INSTANTIATE_TEST_SUITE_P(Boundaries, ExactBudget, testing::Range(0u, 6u));

TEST(Lifecycle, MetadataMatchesSourceSnapshotAndNewIdentityForEveryBuild)
{
    Fixture f({{-0., -8.123456, 0}, {20.123456, +0., 0}});
    std::optional<DatasetId> previous;
    for (auto cell : {1., 0.1, 0.01}) {
        auto result = DensityBuilder::build(f.request(cell)); ASSERT_TRUE(result);
        const auto &m = result.value().dataset.metadata();
        EXPECT_EQ(m.source, f.source.cacheId); EXPECT_EQ(m.sourceMeasurements, f.input.size());
        EXPECT_EQ(std::bit_cast<U64>(m.mapping.cell()), std::bit_cast<U64>(cell));
        EXPECT_EQ(std::bit_cast<U64>(m.mapping.minimum().x), std::bit_cast<U64>(f.source.minimum.x));
        EXPECT_EQ(std::bit_cast<U64>(m.mapping.minimum().y), std::bit_cast<U64>(f.source.minimum.y));
        EXPECT_EQ(std::bit_cast<U64>(m.mapping.maximum().x), std::bit_cast<U64>(f.source.maximum.x));
        EXPECT_EQ(std::bit_cast<U64>(m.mapping.maximum().y), std::bit_cast<U64>(f.source.maximum.y));
        if (previous) EXPECT_NE(*previous, m.id);
        previous = m.id;
        verify(result.value(), oracle(f.input, f.source, cell), f.input.size(), 256);
    }
}

struct Cancellation {
    BuildPhase target;
    bool armed = false;
    U64 callbacks = 0;
    static void progress(const BuildProgress &p, void *context) noexcept {
        auto &s = *static_cast<Cancellation *>(context);
        ++s.callbacks;
        if (p.phase == s.target) s.armed = true;
    }
    static bool cancel(void *context) noexcept { return static_cast<Cancellation *>(context)->armed; }
};
class CancelPhase : public testing::TestWithParam<BuildPhase> {};
TEST_P(CancelPhase, NoCompletedDatasetAndSourcePreserved)
{
    std::vector<cache::Measurement> points;
    for (int i = 0; i < 4500; ++i) points.push_back({double(i % 1300), double((i * 31) % 1400), 0});
    Fixture f(std::move(points)); auto r = f.request();
    const auto original = digest(f.cache->path());
    Cancellation c{GetParam()}; r.progress = Cancellation::progress; r.cancel = Cancellation::cancel; r.context = &c;
    auto result = DensityBuilder::build(r); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, BuildErrorCode::Cancelled);
    EXPECT_EQ(digest(f.cache->path()), original); EXPECT_GT(c.callbacks, 0); f.clean();
}
INSTANTIATE_TEST_SUITE_P(Phases, CancelPhase, testing::Values(BuildPhase::Preflight, BuildPhase::Mapping,
    BuildPhase::SourceOpen, BuildPhase::Scan, BuildPhase::Grouping, BuildPhase::Accumulation,
    BuildPhase::TileCopy, BuildPhase::DensityIngest, BuildPhase::Finish, BuildPhase::Cleanup, BuildPhase::Complete));

class DelayedCancellation : public testing::TestWithParam<BuildPhase> {};
TEST_P(DelayedCancellation, CancelsAfterWorkInsidePhase)
{
    std::vector<cache::Measurement> points(6000, {0, 0, 0}); points.push_back({1500, 2000, 0});
    Fixture f(std::move(points)); auto r = f.request();
    struct Context { BuildPhase phase; bool armed = false; unsigned calls = 0; } c{GetParam()};
    r.context = &c;
    r.progress = [](const BuildProgress &p, void *context) noexcept {
        auto &c = *static_cast<Context *>(context); if (p.phase == c.phase) c.armed = true;
    };
    r.cancel = [](void *context) noexcept {
        auto &c = *static_cast<Context *>(context); return c.armed && ++c.calls == 20;
    };
    auto result = DensityBuilder::build(r); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, BuildErrorCode::Cancelled); EXPECT_EQ(c.calls, 20); f.clean();
}
INSTANTIATE_TEST_SUITE_P(AfterCheckpoints, DelayedCancellation,
    testing::Values(BuildPhase::Scan, BuildPhase::Grouping, BuildPhase::TileCopy));

using Access = DensityBuilderTestAccess;
struct Fault {
    Access::Point target;
    unsigned mutation = 0;
    bool fired = false;
    static std::optional<BuildError> hook(Access::Point point, const QString &path, void *context) noexcept {
        auto &f = *static_cast<Fault *>(context);
        if (point != f.target || f.fired) return {};
        f.fired = true;
        if (!f.mutation) return BuildError{point == Access::Point::Remove ? BuildErrorCode::CleanupFailure
            : point == Access::Point::AfterFinish ? BuildErrorCode::Cancelled : BuildErrorCode::ScratchIO,
            BuildPhase::Accumulation, QStringLiteral("deterministic operation-local injected failure")};
        QFile file(path);
        if (!file.open(QIODevice::ReadWrite)) return BuildError{BuildErrorCode::ScratchIO,
            BuildPhase::Accumulation, QStringLiteral("fixture mutation open failed")};
        if (f.mutation == 1) {
            const auto size = file.size(); if (size > 0) file.resize(size - 1);
        } else if (f.mutation == 2) {
            file.seek(file.size()); file.write("\x00\x00\x00\x00", 4);
        } else {
            file.seek(0); file.write("\xff\xff\xff\xff", 4);
        }
        file.close(); return {};
    }
};
class IOFault : public testing::TestWithParam<Access::Point> {};
TEST_P(IOFault, PreservesPrimaryFailureAndCleansScope)
{
    Fixture f({{0, 0, 0}, {1000, 1000, 0}}); auto r = f.request();
    Fault fault{GetParam()};
    auto result = Access::build(r, Fault::hook, &fault); ASSERT_FALSE(result); EXPECT_TRUE(fault.fired);
    EXPECT_EQ(result.error().code, GetParam() == Access::Point::Remove ? BuildErrorCode::CleanupFailure
        : GetParam() == Access::Point::AfterFinish ? BuildErrorCode::Cancelled : BuildErrorCode::ScratchIO);
    f.clean();
}
INSTANTIATE_TEST_SUITE_P(Spool, IOFault, testing::Values(Access::Point::Create, Access::Point::Append,
    Access::Point::Reopen, Access::Point::Read, Access::Point::Remove, Access::Point::AfterFinish));
class SpoolCorruption : public testing::TestWithParam<unsigned> {};
TEST_P(SpoolCorruption, LengthExtraEventAndInvalidIndex)
{
    Fixture f({{0, 0, 0}, {1200.5, 800.5, 0}});
    Fault fault{Access::Point::Reopen, GetParam()};
    auto result = Access::build(f.request(), Fault::hook, &fault); ASSERT_FALSE(result); EXPECT_TRUE(fault.fired);
    EXPECT_EQ(result.error().code, BuildErrorCode::InconsistentEvents); f.clean();
}
INSTANTIATE_TEST_SUITE_P(Events, SpoolCorruption, testing::Range(1u, 4u));

void inRangeEventCorruption(bool bitFlip)
{
    Fixture f({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}});
    const auto sourceDigest = digest(f.cache->path());
    auto r = f.request(1, 256, 1); // stable append order across three event chunks
    struct Context {
        bool bitFlip, fired = false, changed = false;
        qint64 before = -1, after = -1;
        U64 densityIngestCalls = 0;
    } c{bitFlip};
    auto hook = [](Access::Point point, const QString &path, void *context) noexcept -> std::optional<BuildError> {
        auto &c = *static_cast<Context *>(context);
        if (point == Access::Point::DensityIngest) { ++c.densityIngestCalls; return {}; }
        if (point != Access::Point::Reopen || c.fired) return {};
        c.fired = true;
        QFile file(path);
        if (!file.open(QIODevice::ReadWrite)) return BuildError{BuildErrorCode::ScratchIO,
            BuildPhase::Accumulation, QStringLiteral("event integrity fixture open failed")};
        auto bytes = file.readAll();
        c.before = bytes.size();
        const QByteArray original("\x00\x00\x00\x00\x01\x00\x00\x00\x01\x00\x00\x00", 12);
        if (bytes != original) return BuildError{BuildErrorCode::ScratchIO,
            BuildPhase::Accumulation, QStringLiteral("event integrity fixture order mismatch")};
        if (c.bitFlip) bytes[0] = static_cast<char>(static_cast<unsigned char>(bytes[0]) ^ 1u);
        else bytes = QByteArray("\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00", 12);
        if (!file.seek(0) || file.write(bytes) != bytes.size() || !file.flush())
            return BuildError{BuildErrorCode::ScratchIO, BuildPhase::Accumulation,
                QStringLiteral("event integrity fixture rewrite failed")};
        c.after = file.size(); c.changed = true;
        return {};
    };
    {
        auto previousRequest = r; previousRequest.outputPath = f.dir.filePath("previous.density");
        auto previous = DensityBuilder::build(previousRequest); ASSERT_TRUE(previous);
        const auto previousDigest = digest(previous.value().dataset.path());
        {
            auto result = Access::build(r, +hook, &c);
            ASSERT_TRUE(c.fired); ASSERT_TRUE(c.changed);
            EXPECT_EQ(c.before, 12); EXPECT_EQ(c.after, c.before);
            EXPECT_FALSE(result) << "In-range event corruption must not produce a completed dataset";
            if (result) verify(result.value(), {2, 1, {{{0, 0}, 1}, {{1, 0}, 2}}}, 3, 256);
            else {
                EXPECT_EQ(result.error().code, BuildErrorCode::InconsistentEvents);
                EXPECT_EQ(result.error().phase, BuildPhase::Accumulation);
                EXPECT_TRUE(result.error().group.has_value());
            }
        }
        EXPECT_EQ(c.densityIngestCalls, 0);
        EXPECT_EQ(digest(previous.value().dataset.path()), previousDigest);
        verify(previous.value(), {2, 1, {{{0, 0}, 1}, {{1, 0}, 2}}}, 3, 256);
    }
    EXPECT_EQ(digest(f.cache->path()), sourceDigest);
    f.clean();
}
TEST(EventIntegrity, InRangeBitFlipCannotChangeSpatialCounts) { inRangeEventCorruption(true); }
TEST(EventIntegrity, SameLengthValidEventsCannotChangeSpatialCounts) { inRangeEventCorruption(false); }

class ShortTransfer : public testing::TestWithParam<bool> {};
TEST_P(ShortTransfer, RealPartialTransferIsRejectedAndCleaned)
{
    Fixture f({{0, 0, 0}, {500, 800, 0}});
    struct Context { bool writing; bool fired = false; } c{GetParam()};
    auto transfer = [](bool writing, QFile &file, cache::MutableBytes bytes, void *context) noexcept -> std::optional<qint64> {
        auto &c = *static_cast<Context *>(context);
        if (writing != c.writing || c.fired) return {};
        c.fired = true;
        const auto length = static_cast<qint64>(bytes.size()) - 1;
        return writing ? file.write(reinterpret_cast<const char *>(bytes.data()), length)
            : file.read(reinterpret_cast<char *>(bytes.data()), length);
    };
    auto result = Access::transfer(f.request(), +transfer, &c); ASSERT_FALSE(result); ASSERT_TRUE(c.fired);
    EXPECT_EQ(result.error().code, BuildErrorCode::ScratchIO);
    EXPECT_EQ(result.error().cache->operation, c.writing ? cache::Operation::Write : cache::Operation::Read);
    f.clean();
}
INSTANTIATE_TEST_SUITE_P(Prefix, ShortTransfer, testing::Bool());

TEST(InvalidSource, RelativePathsAreRefused)
{
    Fixture f({{0, 0, 0}});
    for (unsigned variant = 0; variant < 3; ++variant) {
        auto r = f.request();
        if (variant == 0) r.outputPath = "relative-output";
        else if (variant == 1) r.scratchParent = ".";
        else {
            auto location = *r.source.location; location.storageDirectory = L".";
            r.source.location = std::make_shared<const DatasetLocation>(std::move(location));
        }
        auto result = DensityBuilder::build(r); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, BuildErrorCode::InvalidInput); f.clean();
    }
}

class CurrentDirectoryGuard {
public:
    explicit CurrentDirectoryGuard(const QString &path) : previous_(QDir::currentPath())
    { if (!QDir::setCurrent(path)) throw std::runtime_error("fixture current directory change failed"); }
    ~CurrentDirectoryGuard() { if (!QDir::setCurrent(previous_)) ADD_FAILURE() << "Cannot restore fixture current directory"; }
    CurrentDirectoryGuard(const CurrentDirectoryGuard &) = delete;
    CurrentDirectoryGuard &operator=(const CurrentDirectoryGuard &) = delete;
private:
    QString previous_;
};
void replacePath(BuildRequest &r, unsigned field, const QString &path)
{
    if (field == 0) r.outputPath = path;
    else if (field == 1) r.scratchParent = path;
    else {
        auto location = *r.source.location;
        location.storageDirectory.clear(); location.relativeCachePath = path.toStdWString();
        r.source.location = std::make_shared<const DatasetLocation>(std::move(location));
    }
}
QString requestPath(const BuildRequest &r, unsigned field)
{
    return field == 0 ? r.outputPath : field == 1 ? r.scratchParent
        : QString::fromStdWString(r.source.cachePath().wstring());
}
struct PathCallbacks {
    U64 calls = 0;
    QString changeTo;
    bool changed = false;
    static void progress(const BuildProgress &, void *context) noexcept {
        auto &c = *static_cast<PathCallbacks *>(context); ++c.calls;
        c.changed = QDir::setCurrent(c.changeTo);
    }
    static bool cancel(void *context) noexcept { ++static_cast<PathCallbacks *>(context)->calls; return false; }
    static void diagnostic(const BuildError &, void *context) noexcept { ++static_cast<PathCallbacks *>(context)->calls; }
};
class InvalidWindowsPath : public testing::TestWithParam<unsigned> {};
TEST_P(InvalidWindowsPath, RefusedBeforeCallbacksAndCwdChanges)
{
    // Keep both the initial drive CWD and the callback destination inside owned
    // directories on the current checkout's drive (D: in the verified environment).
    Fixture f({{0, 0, 0}}, QDir::current().filePath(".density-path-test-XXXXXX"));
    const auto original = digest(f.cache->path());
    const unsigned field = GetParam() / 5, form = GetParam() % 5;
    {
        QTemporaryDir other(QDir(f.dir.path()).filePath("callback-XXXXXX")); ASSERT_TRUE(other.isValid());
        CurrentDirectoryGuard cwd(f.dir.path());
        auto r = f.request();
        const auto absolute = QDir::fromNativeSeparators(requestPath(r, field));
        ASSERT_GE(absolute.size(), 3); ASSERT_EQ(absolute[1], QLatin1Char(':'));
        const auto drive = absolute.left(2);
        const QString relative = field == 1 ? QStringLiteral(".") : QFileInfo(absolute).fileName();
        QString ambiguous;
        switch (form) {
        case 0: ambiguous = relative; break;
        case 1: ambiguous = drive + relative; break;
        case 2: ambiguous = drive; break;
        case 3: ambiguous = absolute.mid(2); break;
        case 4: ambiguous = QDir::toNativeSeparators(absolute.mid(2)); break;
        }
        replacePath(r, field, ambiguous);
        auto callbacks = std::make_shared<PathCallbacks>(); callbacks->changeTo = other.path();
        r.context = callbacks.get(); r.progress = PathCallbacks::progress; r.cancel = PathCallbacks::cancel;
        r.diagnostic = PathCallbacks::diagnostic; r.diagnosticContext = callbacks;
        {
            auto result = DensityBuilder::build(r);
            EXPECT_FALSE(result) << ambiguous.toStdString();
            if (!result) EXPECT_EQ(result.error().code, BuildErrorCode::InvalidInput);
        }
        EXPECT_EQ(callbacks->calls, 0); EXPECT_FALSE(callbacks->changed);
        EXPECT_EQ(QDir::currentPath(), f.dir.path());
        EXPECT_EQ(QDir(other.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{});
    }
    EXPECT_EQ(digest(f.cache->path()), original); f.clean();
}
std::string invalidWindowsPathName(const testing::TestParamInfo<unsigned> &info)
{
    const char *fields[]{"Output", "Scratch", "Source"};
    const char *forms[]{"Relative", "DriveRelative", "DriveOnly", "SlashRootRelative", "BackslashRootRelative"};
    return std::string(fields[info.param / 5]) + "_" + forms[info.param % 5];
}
INSTANTIATE_TEST_SUITE_P(WindowsForms, InvalidWindowsPath, testing::Range(0u, 15u), invalidWindowsPathName);

class InvalidWindowsSyntax : public testing::TestWithParam<unsigned> {};
TEST_P(InvalidWindowsSyntax, RefusedBeforeCancelAndFilesystemAccess)
{
    Fixture f({{0, 0, 0}});
    const auto drive = QDir::fromNativeSeparators(f.dir.path()).left(2);
    QString bad;
    switch (GetParam()) {
    case 0: bad = QString(); break;
    case 1: bad = QStringLiteral("//server"); break;
    case 2: bad = QStringLiteral("//server/"); break;
    case 3: bad = QStringLiteral("//server//share"); break;
    case 4: bad = QStringLiteral("///share"); break;
    case 5: bad = QStringLiteral(":/resource"); break;
    case 6: bad = QStringLiteral("qrc:/resource"); break;
    case 7: bad = QStringLiteral("\\\\.\\pipe\\density-builder"); break;
    case 8: bad = QStringLiteral("\\\\?\\") + drive + QStringLiteral("relative"); break;
    case 9: bad = drive + QStringLiteral("/file") + QChar(0) + QStringLiteral("suffix"); break;
    case 10: bad = QStringLiteral("/"); break;
    case 11: bad = QStringLiteral("\\"); break;
    case 12: bad = QStringLiteral("Я:/density-builder"); break;
    case 13: bad = QStringLiteral("\\\\?\\UNC\\server"); break;
    case 14: bad = QStringLiteral("\\\\?\\UNC\\server\\\\share"); break;
    }
    for (unsigned field = 0; field < 3; ++field) {
        SCOPED_TRACE(field); auto r = f.request(); replacePath(r, field, bad);
        unsigned calls = 0; r.context = &calls;
        r.progress = [](const BuildProgress &, void *context) noexcept { ++*static_cast<unsigned *>(context); };
        r.cancel = [](void *context) noexcept { ++*static_cast<unsigned *>(context); return true; };
        auto result = DensityBuilder::build(r);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, BuildErrorCode::InvalidInput);
        EXPECT_EQ(calls, 0); f.clean();
    }
}
INSTANTIATE_TEST_SUITE_P(WindowsSyntax, InvalidWindowsSyntax, testing::Range(0u, 15u));

class QualifiedWindowsSyntax : public testing::TestWithParam<unsigned> {};
TEST_P(QualifiedWindowsSyntax, DriveAndUncGrammarReachCancellationWithoutIo)
{
    Fixture f({{0, 0, 0}});
    const auto drive = QDir::fromNativeSeparators(f.dir.path()).left(2);
    QString qualified;
    switch (GetParam()) {
    case 0: qualified = drive + QStringLiteral("/density-абв/file"); break;
    case 1: qualified = drive + QStringLiteral("\\density-абв\\file"); break;
    case 2: qualified = drive.toLower() + QStringLiteral("/density-абв/file"); break;
    case 3: qualified = drive + QStringLiteral("/"); break;
    case 4: qualified = QStringLiteral("//сервер/общая/file"); break;
    case 5: qualified = QStringLiteral("\\\\сервер\\общая\\file"); break;
    case 6: qualified = QStringLiteral("//server/share"); break;
    case 7: qualified = QStringLiteral("\\\\?\\") + drive + QStringLiteral("\\density-абв\\file"); break;
    case 8: qualified = QStringLiteral("\\\\?\\UNC\\сервер\\общая\\file"); break;
    }
    for (unsigned field = 0; field < 3; ++field) {
        SCOPED_TRACE(field); auto r = f.request(); replacePath(r, field, qualified);
        unsigned calls = 0; r.context = &calls;
        r.cancel = [](void *context) noexcept { ++*static_cast<unsigned *>(context); return true; };
        auto result = DensityBuilder::build(r); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, BuildErrorCode::Cancelled); EXPECT_GT(calls, 0); f.clean();
    }
}
INSTANTIATE_TEST_SUITE_P(WindowsSyntax, QualifiedWindowsSyntax, testing::Range(0u, 9u));

class QualifiedWindowsPath : public testing::TestWithParam<unsigned> {};
TEST_P(QualifiedWindowsPath, UnicodeOutputRemainsIndependentOfCallbackCwd)
{
    Fixture f({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, QDir::current().filePath(".density-path-test-XXXXXX"));
    const auto original = digest(f.cache->path());
    {
        QTemporaryDir other(QDir(f.dir.path()).filePath("callback-XXXXXX")); ASSERT_TRUE(other.isValid());
        CurrentDirectoryGuard cwd(f.dir.path());
        auto r = f.request(); r.outputPath = f.dir.filePath(QStringLiteral("карта №1.density"));
        if (GetParam() == 1) r.outputPath = QDir::toNativeSeparators(r.outputPath);
        if (GetParam() == 2) r.outputPath[0] = r.outputPath.at(0).toLower();
        PathCallbacks callbacks; callbacks.changeTo = other.path();
        r.context = &callbacks; r.progress = PathCallbacks::progress;
        {
            auto result = DensityBuilder::build(r); ASSERT_TRUE(result);
            EXPECT_TRUE(callbacks.changed);
            verify(result.value(), {2, 1, {{{0, 0}, 1}, {{1, 0}, 2}}}, 3, 256);
        }
        EXPECT_EQ(QDir(other.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{});
    }
    EXPECT_EQ(digest(f.cache->path()), original); f.clean();
}
INSTANTIATE_TEST_SUITE_P(DriveAbsolute, QualifiedWindowsPath, testing::Range(0u, 3u));

TEST(NumericalEnvironment, UnsupportedRoundModeAndCallbackMutation)
{
    Fixture f({{0, 0, 0}, {3, 3, 0}});
    const auto saved = std::fegetround(); ASSERT_EQ(std::fesetround(FE_DOWNWARD), 0);
    auto result = DensityBuilder::build(f.request());
    EXPECT_EQ(std::fesetround(saved), 0);
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code, BuildErrorCode::Mapping); f.clean();
    auto r = f.request();
    r.progress = [](const BuildProgress &p, void *) noexcept {
        if (p.phase == BuildPhase::Scan) std::fesetround(FE_UPWARD);
    };
    auto changed = DensityBuilder::build(r); EXPECT_EQ(std::fesetround(saved), 0);
    ASSERT_FALSE(changed); EXPECT_EQ(changed.error().code, BuildErrorCode::Mapping); f.clean();
}

class DensityCancel : public testing::TestWithParam<Access::Point> {};
TEST_P(DensityCancel, CancellationInsideStoragePreservesBuilderError)
{
    Fixture f({{0, 0, 0}, {1500, 2000, 0}});
    struct Context { Access::Point target; bool fired = false; } c{GetParam()};
    auto hook = [](Access::Point p, const QString &, void *context) noexcept -> std::optional<BuildError> {
        auto &c = *static_cast<Context *>(context); if (p != c.target) return {};
        c.fired = true; return BuildError{BuildErrorCode::Cancelled, BuildPhase::Finish, QStringLiteral("storage checkpoint cancellation")};
    };
    auto result = Access::build(f.request(), +hook, &c); ASSERT_FALSE(result); EXPECT_TRUE(c.fired);
    EXPECT_EQ(result.error().code, BuildErrorCode::Cancelled);
    EXPECT_EQ(result.error().detail, QStringLiteral("storage checkpoint cancellation")); f.clean();
}
INSTANTIATE_TEST_SUITE_P(StoragePhases, DensityCancel, testing::Values(Access::Point::DensityIngest,
    Access::Point::DensitySort, Access::Point::DensitySeal, Access::Point::DensityReadback));

TEST(Cancellation, DuringEventCountingAfterRead)
{
    std::vector<cache::Measurement> points(6000, {0, 0, 0}); points.push_back({1500, 2000, 0});
    Fixture f(std::move(points)); auto r = f.request(); r.eventCapacity = 16384;
    struct Context { bool read = false; unsigned calls = 0; } c;
    r.context = &c;
    r.cancel = [](void *context) noexcept {
        auto &c = *static_cast<Context *>(context); return c.read && ++c.calls == 4;
    };
    auto hook = [](Access::Point point, const QString &, void *context) noexcept -> std::optional<BuildError> {
        if (point == Access::Point::Read) static_cast<Context *>(context)->read = true;
        return {};
    };
    auto result = Access::build(r, +hook, &c); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, BuildErrorCode::Cancelled); EXPECT_EQ(c.calls, 4); f.clean();
}

TEST(Cancellation, SeveralCheckpointsInsideStorageReadback)
{
    Fixture f({{0, 0, 0}, {1500, 2000, 0}}); auto r = f.request();
    struct Context { bool readback = false; unsigned calls = 0; } c;
    r.context = &c;
    r.cancel = [](void *context) noexcept {
        auto &c = *static_cast<Context *>(context); return c.readback && ++c.calls == 15;
    };
    auto hook = [](Access::Point point, const QString &, void *context) noexcept -> std::optional<BuildError> {
        if (point == Access::Point::DensityReadback) static_cast<Context *>(context)->readback = true;
        return {};
    };
    auto result = Access::build(r, +hook, &c); ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, BuildErrorCode::Cancelled); EXPECT_EQ(c.calls, 15); f.clean();
}

TEST(Lifecycle, RequestValueSnapshotIgnoresCallerMutation)
{
    Fixture f({{0, 0, 0}, {1000, 1200, 0}}); auto r = f.request();
    r.context = &r;
    r.progress = [](const BuildProgress &p, void *context) noexcept {
        if (p.phase != BuildPhase::Preflight) return;
        auto &original = *static_cast<BuildRequest *>(context);
        original.cell = -1; original.groupSide = UINT32_MAX; original.source.location.reset();
        original.resources.workingBytes = 0;
    };
    auto result = DensityBuilder::build(r); ASSERT_TRUE(result);
    verify(result.value(), oracle(f.input, f.source, 1), f.input.size(), 256);
}

TEST(Lifecycle, DiagnosticRetentionSurvivesRequestAndSeparateDatasetMove)
{
    struct Context { unsigned calls = 0; };
    Fixture f({{0, 0, 0}});
    std::weak_ptr<Context> weak;
    std::unique_ptr<PendingDataset> pending;
    QString path;
    {
        auto r = f.request(); auto owner = std::make_shared<Context>(); weak = owner;
        r.diagnosticContext = owner;
        r.diagnostic = [](const BuildError &e, void *context) noexcept {
            if (e.code == BuildErrorCode::CleanupFailure) ++static_cast<Context *>(context)->calls;
        };
        auto result = DensityBuilder::build(r); ASSERT_TRUE(result);
        path = result.value().dataset.path();
        pending = std::make_unique<PendingDataset>(std::move(result.value().dataset));
    }
    ASSERT_FALSE(weak.expired());
    auto observer = weak.lock();
    HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    pending.reset(); EXPECT_EQ(observer->calls, 1);
    EXPECT_EQ(weak.use_count(), 1); observer.reset(); EXPECT_TRUE(weak.expired());
    CloseHandle(lock); ASSERT_TRUE(QFile::remove(path)); f.clean();
}

TEST(Resources, InvalidProfilesAndEncodingOverflow)
{
    Fixture f({{0, 0, 0}});
    for (unsigned variant = 0; variant < 6; ++variant) {
        auto r = f.request();
        switch (variant) {
        case 0: r.eventCapacity = 0; break;
        case 1: r.eventCapacity = std::numeric_limits<U64>::max(); break;
        case 2: r.groupSide = 0; break;
        case 3: r.groupSide = 257; break;
        case 4: r.tileSide = 128; break;
        case 5: r.groupSide = UINT32_MAX; break;
        }
        EXPECT_FALSE(DensityBuilder::build(r)) << variant; f.clean();
    }
}
} // namespace
