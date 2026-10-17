#include "cache/cache_writer.h"
#include "cache_file_test_access.h"
#include "point_stream_sort_test_access.h"

#include <QDirIterator>
#include <QFileInfo>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <numeric>
#include <set>
#include <type_traits>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace cadcontour;
using namespace cadcontour::cache;

namespace cadcontour::cache {
class CacheWriterTestAccess {
public:
    static auto phase(const CacheWriter &w) { return w.phase_; }
    static auto pages(const CacheWriter &w) { return w.pointPages_; }
    static auto count(const CacheWriter &w) { return w.streamCount_; }
    static auto reserve(const CacheWriter &w) { return w.writerReserve_; }
    static auto diskLimit(const CacheWriter &w) { return w.writerDiskLimit_; }
    static auto peakDisk(const CacheWriter &w) { return w.peakDiskBytes_; }
    static auto liveBuffers(const CacheWriter &w)
    {
        return (w.page_ ? format::pageSize : 0) + (w.records_ ? format::pointCapacity * sizeof(PointRecord) : 0)
            + (w.entries_ ? format::indexCapacity * sizeof(IndexEntry) : 0);
    }
    static auto &sorter(CacheWriter &w) { return *w.sorter_; }
    static auto &staged(CacheWriter &w) { return *w.staged_; }
    static auto &descriptors(CacheWriter &w) { return *w.descriptors_; }
    static QString path(const CacheWriter &w) { return w.staged_->temporaryPath(); }
    static const auto &header(const CacheWriter &w) { return w.header_; }
    static const auto &layout(const CacheWriter &w) { return w.layout_; }
    static void hook(CacheWriter &w, Status (*fn)(CacheWriter &, Operation)) { w.ioHook_ = fn; }
    static void setCount(CacheWriter &w, std::uint64_t n) { w.streamCount_ = n; }
    static void setPages(CacheWriter &w, std::uint64_t n) { w.pointPages_ = n; }
    static Status consume(CacheWriter &w, std::span<const PointRecord> records) { return w.consume(records); }
    // Artificial descriptors test index capacity without millions of point records.
    // No point section or publish: the resulting private scratch file is NOT a dataset.
    static Status artificial(CacheWriter &w, std::uint64_t pages, bool fullLayout = false)
    {
        w.sorter_.reset();
        w.header_.metadata.id = CacheId::create();
        w.header_.metadata.pointCount = fullLayout ? (pages - 1) * format::pointCapacity + 1 : pages;
        w.header_.metadata.bounds = {-0., -2, +0., -2};
        w.streamCount_ = w.header_.metadata.pointCount;
        w.streamBounds_ = w.header_.metadata.bounds;
        w.layout_ = calculateLayout(w.streamCount_, 0).value();
        if (!fullLayout) w.layout_.indexOffset = format::headerSize;
        w.pointPages_ = pages;
        for (std::uint64_t i = 0; i < pages; ++i) {
            const auto count = fullLayout && i + 1 < pages ? format::pointCapacity : 1;
            if (auto s = w.writeDescriptor({{-0., -2, +0., -2}, i, count, PageKind::Points}); !s) return s;
        }
        return w.buildIndex();
    }
    static Status build(CacheWriter &w) { return w.buildIndex(); }
    static Status envelope(CacheWriter &w)
    {
        w.phase_ = Phase::Sealing;
        w.layout_ = calculateLayout(w.streamCount_, w.header_.index.pageCount).value();
        MutableBytes bytes{w.page_.get(), format::headerSize};
        if (auto s = encodeHeader(bytes, w.header_); !s) return s;
        if (auto s = w.writeCache(0, bytes); !s) return s;
        const auto &l = w.layout_;
        CompletionFooter f{w.header_.metadata.id, w.streamCount_, l.fileSize, w.pointPages_,
                            w.header_.index.pageCount, l.footerOffset};
        if (auto s = encodeFooter(bytes, f); !s) return s;
        if (auto s = w.writeCache(l.footerOffset, bytes); !s) return s;
        return w.staged_->flush();
    }
    static Status validate(CacheWriter &w, QIODevice &file) { return w.validate(file, w.layout_.fileSize); }
    static Result<IndexEntry> summarize(CacheWriter &w, Bytes page) { return w.summarize(page, 0, 2); }
};
} // namespace cadcontour::cache

namespace {
using W = CacheWriterTestAccess;
using S = PointStreamSortTestAccess;
static_assert(!std::is_copy_constructible_v<PendingCache>);
static_assert(std::is_nothrow_move_constructible_v<PendingCache>);
static_assert(std::is_nothrow_move_assignable_v<PendingCache>);
std::uint64_t bits(double v) { return std::bit_cast<std::uint64_t>(v); }
void sameBounds(BoundingBox a, BoundingBox b)
{
    EXPECT_EQ(bits(a.minX), bits(b.minX)); EXPECT_EQ(bits(a.minY), bits(b.minY));
    EXPECT_EQ(bits(a.maxX), bits(b.maxX)); EXPECT_EQ(bits(a.maxY), bits(b.maxY));
}
// Independent extrema logic, explicitly resolving zero ties without production helpers.
BoundingBox mergeBounds(BoundingBox a, BoundingBox b)
{
    auto low = [](double x, double y) {
        if (x == 0 && y == 0) return std::signbit(x) || std::signbit(y) ? -0. : 0.;
        return std::min(x, y);
    };
    auto high = [](double x, double y) {
        if (x == 0 && y == 0) return std::signbit(x) && std::signbit(y) ? -0. : 0.;
        return std::max(x, y);
    };
    return {low(a.minX, b.minX), low(a.minY, b.minY), high(a.maxX, b.maxX), high(a.maxY, b.maxY)};
}
Bytes asBytes(const QByteArray &bytes)
{
    return {reinterpret_cast<const std::byte *>(bytes.constData()), static_cast<std::size_t>(bytes.size())};
}
QByteArray readAt(QFile &f, std::uint64_t offset, std::size_t count)
{
    EXPECT_TRUE(f.seek(static_cast<qint64>(offset)));
    auto result = f.read(static_cast<qint64>(count));
    EXPECT_EQ(result.size(), static_cast<qsizetype>(count));
    return result;
}
// Independent DFS: no packed grouping assumptions, builder helpers or production reader.
IndexEntry walk(QFile &f, std::uint64_t indexOffset, const IndexDescriptor &index,
                std::uint64_t ordinal, std::uint32_t level, const std::vector<IndexEntry> &points,
                std::set<std::uint64_t> &visited, std::set<std::uint64_t> &pointVisited)
{
    EXPECT_LT(ordinal, index.pageCount);
    if (ordinal >= index.pageCount || !visited.insert(ordinal).second) {
        ADD_FAILURE() << "Cycle or duplicate index page"; return {};
    }
    auto bytes = readAt(f, indexOffset + ordinal * format::pageSize, format::pageSize);
    auto ph = validatePage(asBytes(bytes));
    EXPECT_TRUE(ph);
    if (!ph) return {};
    EXPECT_EQ(ph.value().kind, PageKind::Index); EXPECT_EQ(ph.value().pageId, ordinal);
    EXPECT_EQ(ph.value().level, level);
    IndexEntry result{{}, ordinal, 0, PageKind::Index};
    for (std::size_t i = 0; i < ph.value().count; ++i) {
        auto entry = decodeIndexEntry(asBytes(bytes).subspan(format::pageHeaderSize + i * format::indexEntrySize,
                                                            format::indexEntrySize));
        EXPECT_TRUE(entry); if (!entry) return {};
        auto e = entry.value();
        IndexEntry actual;
        if (level == 1) {
            EXPECT_EQ(e.childKind, PageKind::Points); EXPECT_LT(e.childPage, points.size());
            EXPECT_TRUE(pointVisited.insert(e.childPage).second);
            if (e.childPage >= points.size()) return {};
            actual = points[static_cast<std::size_t>(e.childPage)];
        } else {
            EXPECT_EQ(e.childKind, PageKind::Index);
            actual = walk(f, indexOffset, index, e.childPage, level - 1, points, visited, pointVisited);
        }
        EXPECT_EQ(e.pointCount, actual.pointCount); sameBounds(e.bounds, actual.bounds);
        result.bounds = i ? mergeBounds(result.bounds, actual.bounds) : actual.bounds;
        result.pointCount += actual.pointCount;
    }
    return result;
}

std::vector<Measurement> data(std::size_t n)
{
    const std::array<double, 14> values{-std::numeric_limits<double>::max(), -1000, -1,
        std::nextafter(-1., -2.), -std::numeric_limits<double>::min(),
        -std::numeric_limits<double>::denorm_min(), -0., +0., std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::min(), 1, std::nextafter(1., 2.), 1000, std::numeric_limits<double>::max()};
    std::vector<Measurement> result;
    for (std::size_t i = 0; i < n; ++i)
        result.push_back({values[(i * 7 + i / 14) % values.size()], values[(i * 3) % values.size()], i % 9 ? -0. : 2.});
    return result;
}
void verifyDataset(const QString &path, const std::vector<Measurement> &input)
{
    QFile f(path); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    auto h = decodeHeader(asBytes(readAt(f, 0, format::headerSize))); ASSERT_TRUE(h);
    const auto &meta = h.value().metadata;
    EXPECT_EQ(meta.pointCount, input.size());
    const auto pointPages = input.size() / format::pointCapacity + (input.size() % format::pointCapacity != 0);
    const auto indexOffset = format::headerSize + pointPages * format::pageSize;
    const auto footerOffset = indexOffset + h.value().index.pageCount * format::pageSize;
    EXPECT_EQ(f.size(), static_cast<qint64>(footerOffset + format::footerSize));
    auto footer = decodeFooter(asBytes(readAt(f, footerOffset, format::footerSize))); ASSERT_TRUE(footer);
    ASSERT_TRUE(validateEnvelope(h.value(), footer.value(), static_cast<std::uint64_t>(f.size())));
    EXPECT_EQ(footer.value().pointPages, pointPages);
    bool hasZ = false;
    std::vector<bool> ids(input.size());
    std::vector<IndexEntry> pointEntries;
    std::vector<PointRecord> records;
    for (std::size_t p = 0; p < pointPages; ++p) {
        auto page = readAt(f, format::headerSize + p * format::pageSize, format::pageSize);
        auto ph = validatePage(asBytes(page)); ASSERT_TRUE(ph);
        EXPECT_EQ(ph.value().kind, PageKind::Points); EXPECT_EQ(ph.value().level, 0);
        EXPECT_EQ(ph.value().pageId, p);
        const auto count = std::min(format::pointCapacity, input.size() - p * format::pointCapacity);
        EXPECT_EQ(ph.value().count, count);
        IndexEntry descriptor{{}, p, count, PageKind::Points};
        for (std::size_t i = 0; i < count; ++i) {
            auto r = decodeRecord(asBytes(page).subspan(format::pageHeaderSize + i * format::recordSize, format::recordSize));
            ASSERT_TRUE(r); const auto record = r.value();
            ASSERT_LT(record.id, input.size());
            EXPECT_FALSE(ids[static_cast<std::size_t>(record.id)]); ids[static_cast<std::size_t>(record.id)] = true;
            const auto &m = input[static_cast<std::size_t>(record.id)];
            EXPECT_EQ(bits(record.point.x), bits(m.x)); EXPECT_EQ(bits(record.point.y), bits(m.y));
            hasZ |= m.z != 0;
            const BoundingBox b{m.x, m.y, m.x, m.y};
            descriptor.bounds = i ? mergeBounds(descriptor.bounds, b) : b;
            records.push_back(record);
        }
        pointEntries.push_back(descriptor);
    }
    EXPECT_TRUE(std::all_of(ids.begin(), ids.end(), [](bool v) { return v; }));
    EXPECT_EQ(meta.hasNonZeroZ, hasZ);
    // Numeric IEEE oracle: highest differing coordinate bit chooses the axis; X wins ties.
    for (std::size_t i = 1; i < records.size(); ++i) {
        auto a = records[i - 1], b = records[i];
        const auto dx = std::bit_width(bits(a.point.x == 0 ? 0. : a.point.x) ^ bits(b.point.x == 0 ? 0. : b.point.x));
        const auto dy = std::bit_width(bits(a.point.y == 0 ? 0. : a.point.y) ^ bits(b.point.y == 0 ? 0. : b.point.y));
        EXPECT_TRUE(!dx && !dy ? a.id < b.id : dx >= dy ? a.point.x < b.point.x : a.point.y < b.point.y);
    }
    if (input.empty()) {
        EXPECT_EQ(h.value().index.kind, IndexKind::None); EXPECT_EQ(h.value().index.pageCount, 0);
        EXPECT_EQ(h.value().index.rootPage, format::noPage); EXPECT_EQ(h.value().index.height, 0);
        sameBounds(meta.bounds, {});
    } else {
        std::set<std::uint64_t> visited, pointVisited;
        auto root = walk(f, indexOffset, h.value().index, h.value().index.rootPage, h.value().index.height,
                         pointEntries, visited, pointVisited);
        EXPECT_EQ(visited.size(), h.value().index.pageCount); EXPECT_EQ(pointVisited.size(), pointPages);
        EXPECT_EQ(root.pointCount, input.size()); sameBounds(root.bounds, meta.bounds);
    }
}

class CacheWriterTests : public ::testing::Test {
protected:
    QTemporaryDir parent;
    QString target() const { return QDir(parent.path()).filePath(QString::fromUtf8("готовый cache.pc2d")); }
    ResourceLimits limits(std::uint64_t extra = 8192) const
    {
        const auto writer = 65536 + format::pageSize
            + std::max(format::pointCapacity * sizeof(PointRecord), format::indexCapacity * sizeof(IndexEntry))
            + static_cast<std::uint64_t>(QFileInfo(target()).absoluteFilePath().size() + QDir(parent.path()).absolutePath().size()) * 16;
        const auto sorter = 65536 + static_cast<std::uint64_t>(QDir(parent.path()).absolutePath().size()) * 16;
        return {writer + sorter + extra, 64 * 1024 * 1024, 4};
    }
    std::unique_ptr<CacheWriter> create(std::uint64_t extra = 8192,
                                       CacheWriter::CancelCheck cancel = nullptr, void *context = nullptr)
    {
        auto result = CacheWriter::create(target(), limits(extra), parent.path(), cancel, context);
        EXPECT_TRUE(result) << (result ? "" : result.error().detail.toStdString());
        return result ? std::move(result.value()) : nullptr;
    }
    void clean() { EXPECT_TRUE(QDir(parent.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).empty()); }
    void failed(CacheWriter &w, ErrorCode code)
    {
        auto f = w.finish(); ASSERT_FALSE(f); EXPECT_EQ(f.error().code, code);
        EXPECT_EQ(w.append({}).error().code, code);
        EXPECT_FALSE(QFile::exists(target())); clean();
    }
    void roundTrip(std::size_t n)
    {
        auto writer = create(); ASSERT_TRUE(writer);
        const auto input = data(n);
        ASSERT_TRUE(writer->append({}));
        for (std::size_t i = 0; i < n; i += 77)
            ASSERT_TRUE(writer->append(std::span(input).subspan(i, std::min<std::size_t>(77, n - i))));
        {
            auto result = writer->finish(); ASSERT_TRUE(result) << result.error().detail.toStdString();
            EXPECT_EQ(result.value().metadata().pointCount, n);
            EXPECT_EQ(result.value().path(), target());
            EXPECT_EQ(QDir(parent.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 1);
            verifyDataset(result.value().path(), input);
            EXPECT_FALSE(writer->finish()); EXPECT_FALSE(writer->append({}));
        }
        clean();
    }
};

TEST_F(CacheWriterTests, Empty) { roundTrip(0); }
TEST_F(CacheWriterTests, Single) { roundTrip(1); }
TEST_F(CacheWriterTests, PartialPage) { roundTrip(55); }
TEST_F(CacheWriterTests, FullPage) { roundTrip(2726); }
TEST_F(CacheWriterTests, FullPageAndOne) { roundTrip(2727); }
TEST_F(CacheWriterTests, SeveralPages) { roundTrip(2726 * 3 + 19); }
TEST_F(CacheWriterTests, DegenerateBoundsAndSignedZeros)
{
    auto writer = create(); ASSERT_TRUE(writer);
    std::vector<Measurement> input(3000);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = {i % 2 ? -0. : 0., i % 3 ? 0. : -0., -0.};
    ASSERT_TRUE(writer->append(input));
    auto result = writer->finish(); ASSERT_TRUE(result);
    verifyDataset(result.value().path(), input);
    sameBounds(result.value().metadata().bounds, {-0., -0., +0., +0.});
}

class CacheIndexCapacity : public CacheWriterTests, public ::testing::WithParamInterface<std::uint64_t> {};
TEST_P(CacheIndexCapacity, IndependentArtificialTree)
{
    auto writer = create(); ASSERT_TRUE(writer);
    const auto n = GetParam(); ASSERT_TRUE(W::artificial(*writer, n));
    ASSERT_TRUE(W::staged(*writer).flush());
    QFile f(W::path(*writer)); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const auto &index = W::header(*writer).index;
    const auto leaves = n / format::indexCapacity + (n % format::indexCapacity != 0);
    EXPECT_EQ(index.pageCount, leaves == 1 ? 1 : leaves + 1);
    EXPECT_EQ(index.height, leaves == 1 ? 1 : 2);
    std::vector<IndexEntry> points;
    for (std::uint64_t i = 0; i < n; ++i) points.push_back({{-0., -2, +0., -2}, i, 1, PageKind::Points});
    std::set<std::uint64_t> visited, pointVisited;
    auto root = walk(f, format::headerSize, index, index.rootPage, index.height, points, visited, pointVisited);
    EXPECT_EQ(root.pointCount, n); EXPECT_EQ(visited.size(), index.pageCount); EXPECT_EQ(pointVisited.size(), n);
    EXPECT_LE(W::peakDisk(*writer), W::diskLimit(*writer));
}
INSTANTIATE_TEST_SUITE_P(Boundaries, CacheIndexCapacity, ::testing::Values(1ULL, 3ULL, 1022ULL, 1023ULL, 2045ULL));

TEST_F(CacheWriterTests, AcceptMoveDiscardAndReplacement)
{
    QString accepted;
    {
        auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(2)));
        auto result = writer->finish(); ASSERT_TRUE(result);
        PendingCache moved = std::move(result.value());
        EXPECT_TRUE(result.value().path().isEmpty()); EXPECT_TRUE(QFile::exists(moved.path()));
        accepted = moved.releaseToOwner();
        EXPECT_TRUE(moved.path().isEmpty()); EXPECT_TRUE(moved.releaseToOwner().isEmpty());
    }
    EXPECT_TRUE(QFile::exists(accepted)); verifyDataset(accepted, data(2));
    ASSERT_TRUE(QFile::remove(accepted));
    {
        auto a = create(); ASSERT_TRUE(a);
        auto first = a->finish(); ASSERT_TRUE(first);
        const auto alternate = QDir(parent.path()).filePath(QStringLiteral("second"));
        auto b = CacheWriter::create(alternate, limits(), parent.path()); ASSERT_TRUE(b);
        auto second = b.value()->finish(); ASSERT_TRUE(second);
        first.value() = std::move(second.value());
        EXPECT_FALSE(QFile::exists(target())); EXPECT_TRUE(QFile::exists(alternate));
    }
    clean();
}
TEST_F(CacheWriterTests, AbandonBeforeFinish)
{
    { auto writer = create(400); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(200))); }
    clean();
}
TEST_F(CacheWriterTests, ExistingTargetSurvives)
{
    QFile old(target()); ASSERT_TRUE(old.open(QIODevice::WriteOnly)); ASSERT_EQ(old.write("previous"), 8); old.close();
    auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(1)));
    auto result = writer->finish(); ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::PublishFailed);
    ASSERT_TRUE(old.open(QIODevice::ReadOnly)); EXPECT_EQ(old.readAll(), QByteArray("previous")); old.close();
    EXPECT_EQ(QDir(parent.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).size(), 1);
}
TEST_F(CacheWriterTests, RenameFailure)
{
    auto writer = create(); ASSERT_TRUE(writer);
    // An existing directory at the exact target forces the real rename to fail.
    ASSERT_TRUE(QDir().mkdir(target()));
    auto result = writer->finish(); ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::PublishFailed);
    EXPECT_TRUE(QFileInfo(target()).isDir()); ASSERT_TRUE(QDir().rmdir(target())); clean();
}
TEST_F(CacheWriterTests, InvalidMeasurementIsTerminal)
{
    auto writer = create(); ASSERT_TRUE(writer);
    const std::array<Measurement, 2> invalid{{{1, 2, 0}, {0, 0, std::numeric_limits<double>::infinity()}}};
    auto status = writer->append(invalid); ASSERT_FALSE(status); EXPECT_EQ(status.error().code, ErrorCode::InvalidInput);
    failed(*writer, ErrorCode::InvalidInput);
}
TEST_F(CacheWriterTests, InvalidAndInsufficientBudgets)
{
    const auto good = limits();
    for (const auto bad : {ResourceLimits{0, good.temporaryDiskBytes, 4}, ResourceLimits{1, good.temporaryDiskBytes, 4},
                           ResourceLimits{good.memoryBytes, 0, 4}, ResourceLimits{good.memoryBytes, 16383, 4},
                           ResourceLimits{good.memoryBytes, good.temporaryDiskBytes, 3}}) {
        EXPECT_FALSE(CacheWriter::create(target(), bad, parent.path())); clean();
    }
    EXPECT_FALSE(CacheWriter::create(target(), limits(1), parent.path())); clean();
    EXPECT_FALSE(CacheWriter::create(target(), good, parent.path() + QStringLiteral("/absent"))); clean();
    EXPECT_FALSE(CacheWriter::create(parent.path() + QStringLiteral("/absent/cache"), good, parent.path())); clean();
}
TEST_F(CacheWriterTests, WriterDiskBudgetIncludesCacheAndDescriptors)
{
    auto constrained = limits(); constrained.temporaryDiskBytes = 140000;
    auto writer = CacheWriter::create(target(), constrained, parent.path()); ASSERT_TRUE(writer);
    ASSERT_TRUE(writer.value()->append(data(1)));
    failed(*writer.value(), ErrorCode::ResourceExhausted); // point + index + footer cannot fit the writer half
}
TEST_F(CacheWriterTests, SortDiskBudgetRemainsSeparate)
{
    auto constrained = limits(400); constrained.temporaryDiskBytes = 20000;
    auto writer = CacheWriter::create(target(), constrained, parent.path()); ASSERT_TRUE(writer);
    auto append = writer.value()->append(data(1000)); ASSERT_FALSE(append);
    EXPECT_EQ(append.error().code, ErrorCode::ResourceExhausted); failed(*writer.value(), ErrorCode::ResourceExhausted);
}

struct Cancel {
    CacheWriter *writer = nullptr;
    Phase phase = Phase::Ingest;
    Operation operation = Operation::Calculate;
    int skip = 0;
    bool armed = false;
    inline static Cancel *current = nullptr;
    static bool check(void *context) noexcept { return static_cast<Cancel *>(context)->armed; }
    static Status hook(CacheWriter &w, Operation op)
    {
        auto &c = *current;
        if (W::phase(w) == c.phase && op == c.operation && c.skip-- <= 0) c.armed = true;
        return success();
    }
};
TEST_F(CacheWriterTests, CancellationBeforeAppendAndFinish)
{
    Cancel cancel; auto writer = create(8192, Cancel::check, &cancel); ASSERT_TRUE(writer);
    cancel.armed = true;
    auto append = writer->append({}); ASSERT_FALSE(append); EXPECT_EQ(append.error().code, ErrorCode::Cancelled);
    failed(*writer, ErrorCode::Cancelled);
    EXPECT_FALSE(CacheWriter::create(target(), limits(), parent.path(), Cancel::check, &cancel)); clean();
    cancel.armed = false;
    writer = create(8192, Cancel::check, &cancel); ASSERT_TRUE(writer);
    ASSERT_TRUE(writer->append(data(10))); cancel.armed = true;
    failed(*writer, ErrorCode::Cancelled);
}
TEST_F(CacheWriterTests, CancellationBeforeIndexBuilding)
{
    CacheWriter *current = nullptr;
    auto writer = create(8192, [](void *context) noexcept {
        const auto w = *static_cast<CacheWriter **>(context);
        return w && W::phase(*w) == Phase::IndexBuild;
    }, &current);
    ASSERT_TRUE(writer); current = writer.get(); ASSERT_TRUE(writer->append(data(1)));
    failed(*writer, ErrorCode::Cancelled);
}
TEST_F(CacheWriterTests, CancellationDuringIndexAndBeforePublication)
{
    for (const auto phase : {Phase::PointPages, Phase::IndexBuild, Phase::Sealing}) {
        Cancel cancel; Cancel::current = &cancel;
        auto writer = create(8192, Cancel::check, &cancel); ASSERT_TRUE(writer);
        ASSERT_TRUE(writer->append(data(3000)));
        cancel.phase = phase; cancel.operation = phase == Phase::Sealing ? Operation::Publish : Operation::Write;
        W::hook(*writer, Cancel::hook); failed(*writer, ErrorCode::Cancelled);
    }
}
TEST_F(CacheWriterTests, CancellationDuringSealingReadback)
{
    Cancel cancel; Cancel::current = &cancel;
    auto writer = create(8192, Cancel::check, &cancel); ASSERT_TRUE(writer);
    ASSERT_TRUE(writer->append(data(3000)));
    cancel.phase = Phase::Sealing; cancel.operation = Operation::Read; cancel.skip = 2;
    W::hook(*writer, Cancel::hook); failed(*writer, ErrorCode::Cancelled);
}
TEST_F(CacheWriterTests, CancellationBeforeAndDuringArtificialIndex)
{
    for (const int skip : {0, 1}) {
        Cancel cancel; Cancel::current = &cancel;
        auto writer = create(8192, Cancel::check, &cancel); ASSERT_TRUE(writer);
        cancel.phase = Phase::IndexBuild; cancel.operation = Operation::Write; cancel.skip = skip;
        W::hook(*writer, Cancel::hook);
        EXPECT_FALSE(W::artificial(*writer, 2045));
        EXPECT_FALSE(QFile::exists(target())); writer.reset(); clean();
    }
}

struct WriterFault {
    inline static Phase phase;
    inline static Operation operation;
    inline static int skip;
    inline static bool allocation;
    static Status inject(CacheWriter &w, Operation op)
    {
        if (W::phase(w) != phase || op != operation || skip-- > 0) return success();
        if (allocation) throw std::bad_alloc{};
        auto code = op == Operation::Read ? ErrorCode::ReadFailed : op == Operation::Write ? ErrorCode::WriteFailed
            : op == Operation::Seek ? ErrorCode::SeekFailed : ErrorCode::FlushFailed;
        return makeError(code, phase, op, QStringLiteral("writer fault"));
    }
};
TEST_F(CacheWriterTests, IOWriteReadSeekAndFlushFailures)
{
    struct Case { Phase phase; Operation op; int skip; };
    for (const auto c : {Case{Phase::PointPages, Operation::Write, 2}, // one point page and descriptor already written
                         Case{Phase::PointPages, Operation::Seek, 0},
                         Case{Phase::IndexBuild, Operation::Read, 0}, Case{Phase::IndexBuild, Operation::Write, 0},
                         Case{Phase::IndexBuild, Operation::Flush, 0}, Case{Phase::Sealing, Operation::Write, 0},
                         Case{Phase::Sealing, Operation::Read, 0}}) {
        auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(6000)));
        WriterFault::phase = c.phase; WriterFault::operation = c.op; WriterFault::skip = c.skip;
        WriterFault::allocation = false; W::hook(*writer, WriterFault::inject);
        auto result = writer->finish(); ASSERT_FALSE(result); EXPECT_EQ(result.error().detail, QStringLiteral("writer fault"));
        EXPECT_EQ(writer->append({}).error().detail, result.error().detail);
        EXPECT_FALSE(QFile::exists(target())); clean();
    }
}
TEST_F(CacheWriterTests, ActualStagedFlushFailure)
{
    auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(4)));
    CacheFileTestAccess::setFlush(W::staged(*writer), [](QFileDevice &) -> Status {
        return makeError(ErrorCode::FlushFailed, Phase::FileIO, Operation::Flush, QStringLiteral("flush fault"));
    });
    failed(*writer, ErrorCode::FlushFailed);
}
TEST_F(CacheWriterTests, CleanupFailurePreservesPrimaryErrorAndOwnership)
{
    auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(1)));
    const auto path = W::descriptors(*writer).fileName();
    HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    WriterFault::phase = Phase::PointPages; WriterFault::operation = Operation::Write;
    WriterFault::skip = 0; WriterFault::allocation = false; W::hook(*writer, WriterFault::inject);
    auto result = writer->finish(); EXPECT_FALSE(result);
    if (!result) EXPECT_EQ(result.error().code, ErrorCode::WriteFailed);
    EXPECT_EQ(writer->append({}).error().code, ErrorCode::WriteFailed);
    EXPECT_FALSE(QFile::exists(target()));
    CloseHandle(lock); writer.reset(); clean();
}
TEST_F(CacheWriterTests, BadAllocInSinkIndexAndValidation)
{
    for (const auto phase : {Phase::PointPages, Phase::IndexBuild, Phase::Sealing}) {
        auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(3000)));
        WriterFault::phase = phase; WriterFault::operation = Operation::Read;
        if (phase == Phase::PointPages) WriterFault::operation = Operation::Write;
        WriterFault::skip = 0; WriterFault::allocation = true;
        W::hook(*writer, WriterFault::inject); failed(*writer, ErrorCode::ResourceExhausted);
    }
}
TEST_F(CacheWriterTests, SorterFailureAfterWrittenPrefix)
{
    auto writer = create(400); ASSERT_TRUE(writer);
    static CacheWriter *current; current = writer.get();
    ASSERT_TRUE(writer->append(data(5500)));
    S::hook(W::sorter(*writer), [](PointStreamSort &sorter, Operation op, QFile &) -> Status {
        if (S::phase(sorter) == Phase::Output && op == Operation::Read && W::pages(*current) != 0)
            return makeError(ErrorCode::ReadFailed, Phase::Output, op, QStringLiteral("sorter after sink"));
        return success();
    });
    failed(*writer, ErrorCode::ReadFailed);
    EXPECT_GE(W::pages(*writer), 1);
}
TEST_F(CacheWriterTests, SorterCleanupFailureAfterCompleteSink)
{
    auto writer = create(400); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(5500)));
    const auto lockPath = QDir(S::path(W::sorter(*writer))).filePath(QStringLiteral("blocked"));
    const auto native = reinterpret_cast<LPCWSTR>(lockPath.utf16());
    HANDLE handle = CreateFileW(native, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    auto result = writer->finish();
    EXPECT_FALSE(result); if (!result) EXPECT_EQ(result.error().code, ErrorCode::CleanupFailed);
    EXPECT_EQ(W::count(*writer), 5500); EXPECT_GE(W::pages(*writer), 2);
    EXPECT_FALSE(QFile::exists(target()));
    CloseHandle(handle);
    writer.reset(); // retained sorter cleanup ownership retries after the lock is released
    clean();
}

TEST_F(CacheWriterTests, CorruptDescriptorsPreventPublication)
{
    auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(3000)));
    W::hook(*writer, [](CacheWriter &w, Operation op) -> Status {
        if (W::phase(w) == Phase::IndexBuild && op == Operation::Flush) {
            auto &f = W::descriptors(w); EXPECT_TRUE(f.seek(0)); EXPECT_EQ(f.write("x", 1), 1);
        }
        return success();
    });
    failed(*writer, ErrorCode::CorruptCache);
}
TEST_F(CacheWriterTests, TruncatedDescriptorsPreventPublication)
{
    auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(3000)));
    W::hook(*writer, [](CacheWriter &w, Operation op) -> Status {
        if (W::phase(w) == Phase::IndexBuild && op == Operation::Read)
            EXPECT_TRUE(W::descriptors(w).resize(0));
        return success();
    });
    failed(*writer, ErrorCode::IncompleteCache);
}

struct Tamper {
    inline static int mode;
    static Status hook(CacheWriter &w, Operation op)
    {
        if (W::phase(w) != Phase::Sealing || op != Operation::Publish) return success();
        EXPECT_TRUE(W::staged(w).flush());
        QFile file(W::path(w)); EXPECT_TRUE(file.open(QIODevice::ReadWrite));
        if (mode == 9) { EXPECT_TRUE(file.resize(file.size() - 1)); return success(); }
        const auto offset = mode < 6 ? W::layout(w).indexOffset : mode == 6 || mode == 8 ? 0 : W::layout(w).footerOffset;
        const auto length = mode < 6 ? format::pageSize : format::headerSize;
        auto bytes = readAt(file, offset, length);
        MutableBytes b{reinterpret_cast<std::byte *>(bytes.data()), static_cast<std::size_t>(bytes.size())};
        switch (mode) {
        case 0: binary::putU64(b, format::pageHeaderSize + 32, 1); break; // duplicate/missing point child
        case 1: binary::putDouble(b, format::pageHeaderSize, -1); break; // valid bbox, incorrect data bounds
        case 2: binary::putU64(b, format::pageHeaderSize + 40, 1); break; // valid local count, incorrect subtree
        case 3: binary::putU64(b, 24, 1); break; // wrong page ordinal
        case 4: binary::putU32(b, 32, 2); binary::putU32(b, format::pageHeaderSize + 48, 2); break;
        case 5: binary::putU64(b, format::pageHeaderSize + 32, format::noPage - 1); break;
        case 6: binary::putU64(b, 56, 2999); break;
        case 7: binary::putU64(b, 32, 2999); break;
        case 8: b[40] ^= std::byte{1}; break; // different non-null CacheId
        }
        // Recompute digests so semantic checks, not SHA-256 alone, must reject the file.
        if (mode == 6 || mode == 8) EXPECT_TRUE(sealChecksum(b.first(format::subblockSize)));
        else EXPECT_TRUE(sealChecksum(b));
        EXPECT_TRUE(file.seek(static_cast<qint64>(offset))); EXPECT_EQ(file.write(bytes), bytes.size());
        EXPECT_TRUE(file.flush()); return success();
    }
};
TEST_F(CacheWriterTests, SemanticReadbackRejectsRehashedDamage)
{
    for (int mode = 0; mode < 10; ++mode) {
        auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(3000)));
        Tamper::mode = mode; W::hook(*writer, Tamper::hook);
        failed(*writer, mode == 9 ? ErrorCode::IncompleteCache : ErrorCode::CorruptCache);
    }
}
TEST_F(CacheWriterTests, HigherLevelSealingReadbackAndRehashedDamage)
{
    // Artificial descriptors and an unwritten point section exercise only the writer's
    // bounded index validator. Nothing here is published or called a complete dataset.
    for (int mode = -1; mode < 5; ++mode) {
        auto budget = limits(); budget.temporaryDiskBytes = 256 * 1024 * 1024;
        auto created = CacheWriter::create(target(), budget, parent.path()); ASSERT_TRUE(created);
        auto writer = std::move(created.value());
        ASSERT_TRUE(W::artificial(*writer, 1023, true)); ASSERT_TRUE(W::envelope(*writer));
        QFile file(W::path(*writer)); ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        if (mode >= 0) {
            const auto root = W::header(*writer).index.rootPage;
            const auto offset = W::layout(*writer).indexOffset + root * format::pageSize;
            auto bytes = readAt(file, offset, format::pageSize);
            MutableBytes b{reinterpret_cast<std::byte *>(bytes.data()), static_cast<std::size_t>(bytes.size())};
            if (mode == 0) binary::putU64(b, format::pageHeaderSize + 32, root); // cycle
            if (mode == 1) binary::putU32(b, 32, 3); // wrong child level relationship
            if (mode == 2) binary::putU64(b, format::pageHeaderSize + 40, 1);
            if (mode == 3) binary::putDouble(b, format::pageHeaderSize + 16, 1); // parent too wide, valid local bbox
            if (mode == 4) binary::putU64(b, format::pageHeaderSize + format::indexEntrySize + 32, 0); // duplicate
            ASSERT_TRUE(sealChecksum(b)); ASSERT_TRUE(file.seek(static_cast<qint64>(offset)));
            ASSERT_EQ(file.write(bytes), bytes.size()); ASSERT_TRUE(file.flush());
        }
        auto validated = W::validate(*writer, file);
        if (mode == -1) EXPECT_TRUE(validated);
        else { ASSERT_FALSE(validated); EXPECT_EQ(validated.error().code, ErrorCode::CorruptCache); }
        file.close(); writer.reset(); clean();
    }
}
TEST_F(CacheWriterTests, ReentryDoesNotModifyDataset)
{
    auto writer = create(); ASSERT_TRUE(writer); ASSERT_TRUE(writer->append(data(3000)));
    W::hook(*writer, [](CacheWriter &w, Operation op) -> Status {
        if (op == Operation::Write) {
            EXPECT_EQ(w.append({}).error().code, ErrorCode::InvalidState);
            EXPECT_EQ(w.finish().error().code, ErrorCode::InvalidState);
        }
        return success();
    });
    auto result = writer->finish(); ASSERT_TRUE(result); verifyDataset(result.value().path(), data(3000));
}
TEST_F(CacheWriterTests, CheckedCountsAndOffsets)
{
    auto writer = create(); ASSERT_TRUE(writer);
    W::setCount(*writer, UINT64_MAX);
    const std::array<PointRecord, 1> record{{{0, {1, 1}}}};
    EXPECT_EQ(W::consume(*writer, record).error().code, ErrorCode::ResourceExhausted);
    writer.reset(); clean();
    writer = create(); ASSERT_TRUE(writer); W::setPages(*writer, UINT64_MAX - 1);
    std::vector<PointRecord> page(format::pointCapacity, {0, {1, 1}});
    EXPECT_FALSE(W::consume(*writer, page)); EXPECT_FALSE(QFile::exists(target()));
    writer.reset(); clean();
    writer = create(); ASSERT_TRUE(writer);
    std::array<std::byte, format::pageSize> encoded{};
    const std::array<IndexEntry, 2> overflowing{{{{}, 0, UINT64_MAX, PageKind::Index},
                                               {{}, 1, 1, PageKind::Index}}};
    ASSERT_TRUE(encodeIndexPage(encoded, 0, 2, overflowing));
    EXPECT_EQ(W::summarize(*writer, encoded).error().code, ErrorCode::ResourceExhausted);
}
TEST_F(CacheWriterTests, MultiGenerationIntegrationAndNativeResourceBounds)
{
    { auto warmup = create(400); ASSERT_TRUE(warmup); }
    static DWORD baseline; ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &baseline));
    auto writer = create(400); ASSERT_TRUE(writer);
    auto &sorter = W::sorter(*writer);
    ASSERT_EQ(S::fan(sorter), 2);
    ASSERT_TRUE(writer->append(data(2800)));
    EXPECT_GT(S::stats(sorter).initialRuns, 2);
    static CacheWriter *current; current = writer.get();
    static std::uint64_t generations; generations = 0;
    S::hook(sorter, [](PointStreamSort &s, Operation op, QFile &) -> Status {
        generations = std::max(generations, S::stats(s).mergeGenerations);
        EXPECT_LE(S::stats(s).peakBufferBytes + S::fixed(s) + W::reserve(*current),
                  W::reserve(*current) + S::fixed(s) + 400);
        DWORD handles = 0; EXPECT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handles));
        EXPECT_LE(handles, baseline + 4);
        if (S::phase(s) == Phase::Merge && op == Operation::Flush) {
            auto parentPath = QFileInfo(S::path(s)).absolutePath();
            QDirIterator files(parentPath, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
            std::uint64_t bytes = 0; std::size_t count = 0;
            while (files.hasNext()) { files.next(); bytes += static_cast<std::uint64_t>(files.fileInfo().size()); ++count; }
            EXPECT_LE(bytes, 64ULL * 1024 * 1024); EXPECT_LE(count, 4);
        }
        return success();
    });
    W::hook(*writer, [](CacheWriter &w, Operation) -> Status {
        DWORD handles = 0; EXPECT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handles));
        EXPECT_LE(handles, baseline + 4);
        EXPECT_LE(W::peakDisk(w), W::diskLimit(w));
        EXPECT_LE(W::liveBuffers(w), format::pageSize + format::pointCapacity * sizeof(PointRecord));
        return success();
    });
    auto result = writer->finish(); ASSERT_TRUE(result); EXPECT_GE(generations, 2);
    EXPECT_LE(W::peakDisk(*writer), limits(400).temporaryDiskBytes / 2);
    verifyDataset(result.value().path(), data(2800));
}
} // namespace
