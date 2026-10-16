#include "cache/point_cache_format.h"

#include <QByteArray>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

using namespace cadcontour::cache;

namespace {
void hex(MutableBytes b, std::size_t offset, const char *text)
{
    const auto bytes = QByteArray::fromHex(text);
    ASSERT_LE(offset + static_cast<std::size_t>(bytes.size()), b.size());
    std::memcpy(b.data() + offset, bytes.constData(), static_cast<std::size_t>(bytes.size()));
}
CacheId fixedId()
{
    CacheId id; hex(id.bytes, 0, "00112233445566778899aabbccddeeff"); return id;
}
CacheHeader sampleHeader()
{
    return {{fixedId(), 1, {-1, -2, 3, 4}, true}, {IndexKind::BoundingBoxTree, 1, 0, 1}};
}
CompletionFooter sampleFooter()
{
    return {fixedId(), 1, 139264, 1, 1, 135168};
}

// Sparse fixtures assembled without production codec. Digests below are from
// Python hashlib/struct, independently of Qt and the C++ encoder.
std::array<std::byte, 4096> headerFixture()
{
    std::array<std::byte, 4096> b{};
    hex(b, 0, "50433244444154410100000004030201001000000000010018000000010000000100000001000000");
    hex(b, 40, "00112233445566778899aabbccddeeff0100000000000000");
    hex(b, 64, "000000000000f0bf00000000000000c000000000000008400000000000001040");
    hex(b, 96, "00100000000000000100000000000000");
    hex(b, 2048, "50433244494e4458010000000000000000000100400000000100000001000000");
    hex(b, 2080, "00100100000000000100000000000000000000000000000001000000");
    return b;
}
std::array<std::byte, 4096> footerFixture()
{
    std::array<std::byte, 4096> b{};
    hex(b, 0, "50433244444f4e45010000000100000000112233445566778899aabbccddeeff");
    hex(b, 32, "01000000000000000020020000000000010000000000000001000000000000000010020000000000");
    return b;
}
std::array<std::byte, 65536> pointFixture()
{
    std::array<std::byte, 65536> b{};
    hex(b, 0, "50433244504147450100000001000000000000000100000007000000000000000000000018000000");
    hex(b, 64, "2a00000000000000000000000000f03f0000000000000080");
    return b;
}
std::array<std::byte, 65536> indexFixture()
{
    std::array<std::byte, 65536> b{};
    hex(b, 0, "50433244504147450100000002000000000000000100000003000000000000000100000040000000");
    hex(b, 64, "000000000000f0bf00000000000000c000000000000008400000000000001040");
    hex(b, 96, "0700000000000000010000000000000001000000");
    return b;
}
} // namespace

TEST(CacheChecksum, IndependentSha256AbcVector)
{
    std::array<std::byte, 35> b{};
    hex(b, 0, "616263");
    ASSERT_TRUE(sealChecksum(b));
    std::array<std::byte, 32> expected{};
    hex(expected, 0, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), b.begin() + 3));
    EXPECT_TRUE(verifyChecksum(b));
    b[1] ^= std::byte{1}; EXPECT_FALSE(verifyChecksum(b));
    b[1] ^= std::byte{1}; b[34] ^= std::byte{1}; EXPECT_FALSE(verifyChecksum(b));
    EXPECT_FALSE(sealChecksum(MutableBytes{b}.first(31)));
    EXPECT_FALSE(verifyChecksum(Bytes{b}.first(31)));
}

TEST(CacheFormat, HeaderGoldenFixture)
{
    auto fixture = headerFixture();
    hex(fixture, 2016, "fd6f2351bfa891281a28a9ad0164007b83367bf77896b6baefad2d0e180809c7");
    hex(fixture, 4064, "053d4a555e14f861d0de5e023916786e32ccbcfa7841708ab9b5b225bb2121b2");
    const auto decoded = decodeHeader(fixture);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().metadata.id, fixedId());
    EXPECT_EQ(decoded.value().metadata.pointCount, 1);
    EXPECT_EQ(decoded.value().metadata.bounds.minX, -1);
    EXPECT_EQ(decoded.value().metadata.bounds.minY, -2);
    EXPECT_EQ(decoded.value().metadata.bounds.maxX, 3);
    EXPECT_EQ(decoded.value().metadata.bounds.maxY, 4);
    EXPECT_TRUE(decoded.value().metadata.hasNonZeroZ);
    EXPECT_EQ(decoded.value().index.pageCount, 1);
    EXPECT_EQ(decoded.value().index.rootPage, 0);
    EXPECT_EQ(decoded.value().index.height, 1);
    std::array<std::byte, 4096> encoded{};
    ASSERT_TRUE(encodeHeader(encoded, sampleHeader()));
    EXPECT_EQ(encoded, fixture);
}

TEST(CacheFormat, FooterGoldenFixture)
{
    auto fixture = footerFixture();
    hex(fixture, 4064, "31d773a1589e9c0ad67da3a7241de83a65d10c5d395df37c99f952d3f614f228");
    const auto decoded = decodeFooter(fixture);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().id, fixedId());
    EXPECT_EQ(decoded.value().fileSize, 139264);
    EXPECT_EQ(decoded.value().footerOffset, 135168);
    std::array<std::byte, 4096> encoded{};
    ASSERT_TRUE(encodeFooter(encoded, sampleFooter()));
    EXPECT_EQ(encoded, fixture);
    EXPECT_TRUE(validateEnvelope(sampleHeader(), decoded.value(), 139264));
}

TEST(CacheFormat, PointAndIndexPageGoldenFixtures)
{
    auto points = pointFixture(); auto index = indexFixture();
    hex(points, 65504, "389b745d052ae92ecdbb19550cde58b17bbd454cfd1f0222d11d9528ab746553");
    hex(index, 65504, "660729afee161a57f45e3af285de12a90733ac548fa5b9b340532760cb2edc18");
    const auto p = validatePage(points); const auto i = validatePage(index);
    ASSERT_TRUE(p); ASSERT_TRUE(i);
    EXPECT_EQ(p.value().pageId, 7); EXPECT_EQ(p.value().count, 1);
    EXPECT_EQ(i.value().kind, PageKind::Index); EXPECT_EQ(i.value().level, 1);
    const auto e = decodeIndexEntry(Bytes{index}.subspan(64, 64));
    ASSERT_TRUE(e); EXPECT_EQ(e.value().childPage, 7); EXPECT_EQ(e.value().pointCount, 1);
    std::array<std::byte, 65536> encoded{};
    const PointRecord record{42, {1, -0.0}};
    ASSERT_TRUE(encodePointPage(encoded, 7, {&record, 1}));
    EXPECT_EQ(encoded, points);
    const IndexEntry entry{{-1, -2, 3, 4}, 7, 1, PageKind::Points};
    ASSERT_TRUE(encodeIndexPage(encoded, 3, 1, {&entry, 1}));
    EXPECT_EQ(encoded, index);
}

TEST(CacheFormat, HeaderCompatibilityAndCorruption)
{
    for (auto offset : {8u, 12u, 28u, 32u, 36u, 2056u, 2060u, 2072u, 2076u}) {
        auto b = headerFixture();
        // All these fields use values other than 0x80 in v1.
        b[offset] = std::byte{0x80};
        ASSERT_TRUE(sealChecksum(MutableBytes{b}.first(2048)));
        ASSERT_TRUE(sealChecksum(MutableBytes{b}.subspan(2048)));
        EXPECT_EQ(decodeHeader(b).error().code, ErrorCode::UnsupportedFormat) << offset;
    }
    for (auto offset : {0u, 16u, 20u, 24u, 96u, 104u, 112u, 2048u, 2064u, 2068u, 2080u, 2096u, 2104u, 2108u}) {
        std::array<std::byte, 4096> b{};
        ASSERT_TRUE(encodeHeader(b, sampleHeader()));
        b[offset] ^= std::byte{0x80};
        ASSERT_TRUE(sealChecksum(MutableBytes{b}.first(2048)));
        ASSERT_TRUE(sealChecksum(MutableBytes{b}.subspan(2048)));
        EXPECT_EQ(decodeHeader(b).error().code, ErrorCode::CorruptCache) << offset;
    }
    std::array<std::byte, 4096> b{};
    ASSERT_TRUE(encodeHeader(b, sampleHeader()));
    b[120] ^= std::byte{1}; EXPECT_EQ(decodeHeader(b).error().code, ErrorCode::CorruptCache);
    ASSERT_TRUE(encodeHeader(b, sampleHeader()));
    b[4095] ^= std::byte{1}; EXPECT_FALSE(decodeHeader(b));
    EXPECT_EQ(decodeHeader(Bytes{b}.first(4095)).error().code, ErrorCode::IncompleteCache);
    EXPECT_FALSE(encodeHeader(MutableBytes{b}.first(4095), sampleHeader()));
}

TEST(CacheFormat, MetadataAndEnvelopeConstraints)
{
    auto h = sampleHeader(); auto f = sampleFooter();
    EXPECT_EQ(validateEnvelope(h, f, 139263).error().code, ErrorCode::IncompleteCache);
    EXPECT_EQ(validateEnvelope(h, f, 139265).error().code, ErrorCode::CorruptCache);
    f.id = CacheId::create(); EXPECT_FALSE(validateEnvelope(h, f, 139264));
    f = sampleFooter(); f.pointCount = 2; EXPECT_FALSE(validateEnvelope(h, f, 139264));
    f = sampleFooter(); f.pointPages = 2; EXPECT_FALSE(validateEnvelope(h, f, 139264));
    std::array<std::byte, 4096> b{};
    h.metadata.id = {}; EXPECT_FALSE(encodeHeader(b, h));
    h = sampleHeader(); h.metadata.bounds.minX = 4; EXPECT_FALSE(encodeHeader(b, h));
    h = sampleHeader(); h.index.rootPage = 1; EXPECT_FALSE(encodeHeader(b, h));
    h = sampleHeader(); h.index.height = 0; EXPECT_FALSE(encodeHeader(b, h));
    h = {{fixedId(), 0, {}, false}, {}};
    ASSERT_TRUE(encodeHeader(b, h)); EXPECT_TRUE(decodeHeader(b));
    CompletionFooter empty{fixedId(), 0, 8192, 0, 0, 4096};
    EXPECT_TRUE(validateEnvelope(h, empty, 8192));
    h.metadata.bounds.minX = -0.0; EXPECT_FALSE(encodeHeader(b, h));
    h.metadata.bounds = {}; h.metadata.hasNonZeroZ = true; EXPECT_FALSE(encodeHeader(b, h));
}

TEST(CacheFormat, FooterNegativeCases)
{
    std::array<std::byte, 4096> b{};
    ASSERT_TRUE(encodeFooter(b, sampleFooter()));
    b[12] = std::byte{0}; ASSERT_TRUE(sealChecksum(b));
    EXPECT_EQ(decodeFooter(b).error().code, ErrorCode::IncompleteCache);
    ASSERT_TRUE(encodeFooter(b, sampleFooter()));
    b[8] = std::byte{2}; ASSERT_TRUE(sealChecksum(b));
    EXPECT_EQ(decodeFooter(b).error().code, ErrorCode::UnsupportedFormat);
    for (auto offset : {0u, 40u, 48u, 56u, 64u, 72u}) {
        ASSERT_TRUE(encodeFooter(b, sampleFooter()));
        b[offset] ^= std::byte{0x80}; ASSERT_TRUE(sealChecksum(b));
        EXPECT_FALSE(decodeFooter(b)) << offset;
    }
    EXPECT_FALSE(decodeFooter({}));
    EXPECT_FALSE(encodeFooter(MutableBytes{b}.first(4095), sampleFooter()));
}

TEST(CacheFormat, PageCapacityPaddingAndReservedBytes)
{
    EXPECT_EQ(format::pointCapacity, 2726); EXPECT_EQ(format::indexCapacity, 1022);
    std::vector<PointRecord> records(2726, PointRecord{0, {1, 2}});
    std::array<std::byte, 65536> b{};
    ASSERT_TRUE(encodePointPage(b, 0, records)); EXPECT_TRUE(validatePage(b));
    records.push_back({}); EXPECT_FALSE(encodePointPage(b, 0, records));
    const PointRecord r{1, {1, 2}};
    for (auto offset : {40u, 63u, 88u, 65503u}) {
        ASSERT_TRUE(encodePointPage(b, 0, {&r, 1}));
        b[offset] = std::byte{1}; ASSERT_TRUE(sealChecksum(b));
        EXPECT_EQ(validatePage(b).error().code, ErrorCode::CorruptCache) << offset;
    }
    ASSERT_TRUE(encodePointPage(b, 0, {&r, 1}));
    b[70] ^= std::byte{1}; EXPECT_FALSE(validatePage(b));
    EXPECT_FALSE(encodePointPage(b, 0, {}));
    EXPECT_FALSE(encodePointPage(b, format::noPage, {&r, 1}));
    EXPECT_FALSE(validatePage(Bytes{b}.first(65535)));
}

TEST(CacheFormat, PageSemanticAndCompatibilityChecks)
{
    std::array<std::byte, 65536> b{};
    const PointRecord r{1, {1, 2}};
    for (auto offset : {8u, 12u, 16u}) {
        ASSERT_TRUE(encodePointPage(b, 0, {&r, 1}));
        b[offset] = std::byte{0x80}; ASSERT_TRUE(sealChecksum(b));
        EXPECT_EQ(validatePage(b).error().code, ErrorCode::UnsupportedFormat);
    }
    for (auto offset : {0u, 20u, 32u, 36u}) {
        ASSERT_TRUE(encodePointPage(b, 0, {&r, 1}));
        b[offset] ^= std::byte{0x80}; ASSERT_TRUE(sealChecksum(b));
        EXPECT_FALSE(validatePage(b));
    }
    ASSERT_TRUE(encodePointPage(b, 0, {&r, 1}));
    binary::putU64(b, 72, 0x7ff0000000000000ULL); ASSERT_TRUE(sealChecksum(b));
    EXPECT_FALSE(validatePage(b));
    IndexEntry entry{{0, 0, 1, 1}, 0, 1, PageKind::Points};
    EXPECT_FALSE(encodeIndexPage(b, 0, 2, {&entry, 1}));
    EXPECT_FALSE(encodeIndexPage(b, 0, 0, {&entry, 1}));
    ASSERT_TRUE(encodeIndexPage(b, 0, 1, {&entry, 1}));
    binary::putU32(b, 32, 2); ASSERT_TRUE(sealChecksum(b)); EXPECT_FALSE(validatePage(b));
    entry.childKind = PageKind::Index;
    ASSERT_TRUE(encodeIndexPage(b, 0, 2, {&entry, 1})); EXPECT_TRUE(validatePage(b));
}

TEST(CacheFormat, IndexEntryValidation)
{
    std::array<std::byte, 64> b{};
    IndexEntry e{{-1, -2, 3, 4}, 0, 1, PageKind::Points};
    ASSERT_TRUE(encodeIndexEntry(b, e));
    b[52] = std::byte{1}; EXPECT_FALSE(decodeIndexEntry(b));
    ASSERT_TRUE(encodeIndexEntry(b, e));
    b[48] = std::byte{3}; EXPECT_EQ(decodeIndexEntry(b).error().code, ErrorCode::UnsupportedFormat);
    e.pointCount = 0; EXPECT_FALSE(encodeIndexEntry(b, e));
    e.pointCount = 1; e.childPage = format::noPage; EXPECT_FALSE(encodeIndexEntry(b, e));
    EXPECT_FALSE(decodeIndexEntry(Bytes{b}.first(63)));
}

TEST(CacheFormat, IndexCapacityAndInvalidPageBuffers)
{
    std::array<std::byte, 65536> b{};
    const IndexEntry entry{{0, 0, 1, 1}, 0, 1, PageKind::Points};
    std::vector<IndexEntry> entries(1022, entry);
    ASSERT_TRUE(encodeIndexPage(b, 0, 1, entries)); EXPECT_TRUE(validatePage(b));
    entries.push_back(entry); EXPECT_FALSE(encodeIndexPage(b, 0, 1, entries));
    EXPECT_FALSE(encodeIndexPage(b, 0, 1, {}));
    EXPECT_FALSE(encodeIndexPage(MutableBytes{b}.first(65535), 0, 1, {&entry, 1}));
    const PointRecord point{0, {1, 2}};
    EXPECT_FALSE(encodePointPage(MutableBytes{b}.first(65535), 0, {&point, 1}));
    EXPECT_FALSE(encodeIndexEntry(MutableBytes{b}.first(63), entry));
}

TEST(CacheFormat, InvalidMetadataBitsAndOverflowingServiceFields)
{
    std::array<std::byte, 4096> b{};
    for (auto offset : {64u, 72u, 80u, 88u}) {
        ASSERT_TRUE(encodeHeader(b, sampleHeader()));
        binary::putU64(b, offset, 0x7ff0000000000000ULL);
        ASSERT_TRUE(sealChecksum(MutableBytes{b}.first(2048)));
        EXPECT_EQ(decodeHeader(b).error().code, ErrorCode::CorruptCache);
    }
    for (auto offset : {56u, 2088u}) {
        ASSERT_TRUE(encodeHeader(b, sampleHeader()));
        binary::putU64(b, offset, UINT64_MAX);
        ASSERT_TRUE(sealChecksum(MutableBytes{b}.first(2048)));
        ASSERT_TRUE(sealChecksum(MutableBytes{b}.subspan(2048)));
        EXPECT_EQ(decodeHeader(b).error().code, ErrorCode::CorruptCache);
    }
    ASSERT_TRUE(encodeHeader(b, sampleHeader()));
    std::fill_n(b.begin() + 40, 16, std::byte{0});
    ASSERT_TRUE(sealChecksum(MutableBytes{b}.first(2048)));
    EXPECT_EQ(decodeHeader(b).error().code, ErrorCode::CorruptCache);
}
