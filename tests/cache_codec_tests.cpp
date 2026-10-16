#include "cache/point_cache_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

using namespace cadcontour;
using namespace cadcontour::cache;

TEST(CacheCodec, IndependentLittleEndianBytesAndUnalignedAccess)
{
    std::array<std::byte, 17> b{};
    ASSERT_TRUE(binary::putU64(b, 1, 0x0123456789abcdefULL));
    const std::array<std::byte, 8> golden{std::byte{0xef}, std::byte{0xcd}, std::byte{0xab}, std::byte{0x89},
                                        std::byte{0x67}, std::byte{0x45}, std::byte{0x23}, std::byte{0x01}};
    EXPECT_TRUE(std::equal(golden.begin(), golden.end(), b.begin() + 1));
    EXPECT_EQ(binary::getU64(golden, 0), 0x0123456789abcdefULL);
    ASSERT_TRUE(binary::putU32(b, 9, 0xfedcba98));
    EXPECT_EQ(b[9], std::byte{0x98}); EXPECT_EQ(b[12], std::byte{0xfe});
    EXPECT_EQ(binary::getU32(b, 9), 0xfedcba98);
}

TEST(CacheCodec, BufferBoundsAreCheckedBeforeMutation)
{
    std::array<std::byte, 8> b{};
    const auto original = b;
    EXPECT_FALSE(binary::putU64(b, 1, 42));
    EXPECT_FALSE(binary::putU32(b, SIZE_MAX, 42));
    EXPECT_FALSE(binary::putDouble(b, 8, 42));
    EXPECT_EQ(b, original);
    EXPECT_FALSE(binary::getU64(b, 1));
    EXPECT_FALSE(binary::getU32(b, SIZE_MAX));
    EXPECT_FALSE(binary::getDouble({}, 0));
}

TEST(CacheCodec, RecordGoldenBytes)
{
    const std::array<unsigned char, 24> golden{
        0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01,
        0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0,0,0,0,0x80};
    const Bytes fixture{reinterpret_cast<const std::byte *>(golden.data()), golden.size()};
    const auto decoded = decodeRecord(fixture);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().id, 0x0123456789abcdefULL);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(decoded.value().point.x), 0x3ff0000000000000ULL);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(decoded.value().point.y), 0x8000000000000000ULL);
    std::array<std::byte, 24> b{};
    ASSERT_TRUE(encodeRecord(b, {0x0123456789abcdefULL, {1, -0.0}}));
    EXPECT_TRUE(std::equal(b.begin(), b.end(), fixture.begin()));
}

TEST(CacheCodec, FiniteCoordinateEdgeCasesSurviveBitwise)
{
    const std::uint64_t cases[]{0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL,
        0x000fffffffffffffULL, 0x0010000000000000ULL, 0x3ff0000000000000ULL,
        0x3ff0000000000001ULL, 0x3fefffffffffffffULL, 0xc00921fb54442d18ULL,
        0x7fefffffffffffffULL, 0xffefffffffffffffULL, 0x01a56e1fc2f8f359ULL};
    for (auto bits : cases) {
        SCOPED_TRACE(bits);
        const double v = std::bit_cast<double>(bits);
        std::array<std::byte, 24> b{};
        ASSERT_TRUE(encodeRecord(b, {UINT64_MAX, {v, v}}));
        // Independent integer expectation catches a symmetric double codec error.
        EXPECT_EQ(binary::getU64(b, 8), bits);
        EXPECT_EQ(binary::getU64(b, 16), bits);
        const auto r = decodeRecord(b);
        ASSERT_TRUE(r);
        EXPECT_EQ(r.value().id, UINT64_MAX);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(r.value().point.x), bits);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(r.value().point.y), bits);
    }
}

TEST(CacheCodec, RawBitsAndDatasetValidityAreSeparate)
{
    for (auto bits : {0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000001ULL,
                      0x7ff0000000000001ULL}) {
        std::array<std::byte, 24> b{};
        ASSERT_TRUE(binary::putU64(b, 8, bits));
        const auto raw = binary::getDouble(b, 8);
        ASSERT_TRUE(raw);
        EXPECT_EQ(std::bit_cast<std::uint64_t>(*raw), bits);
        EXPECT_EQ(decodeRecord(b).error().code, ErrorCode::CorruptCache);
        EXPECT_EQ(encodeRecord(b, {0, {*raw, 0}}).error().code, ErrorCode::InvalidInput);
        EXPECT_EQ(encodeRecord(b, {0, {0, *raw}}).error().code, ErrorCode::InvalidInput);
    }
    std::array<std::byte, 25> b{};
    EXPECT_FALSE(encodeRecord(b, {}));
    EXPECT_FALSE(decodeRecord(b));
    EXPECT_FALSE(decodeRecord({}));
}

TEST(CacheTypes, IdentityDoesNotDependOnCoordinatesOrPath)
{
    const auto a = CacheId::create(); const auto b = CacheId::create();
    EXPECT_FALSE(a.isNull()); EXPECT_FALSE(b.isNull()); EXPECT_NE(a, b);
    EXPECT_EQ(a, CacheId{a.bytes});
    const PointRecord first{0, {1, 2}}, repeated{1, {1, 2}};
    EXPECT_NE(first.id, repeated.id);
    EXPECT_EQ(sizeof(RecordId), 8);
    EXPECT_TRUE(CacheId{}.isNull());
}

TEST(CacheTypes, ErrorDiagnosticsAreBounded)
{
    const auto e = makeError(ErrorCode::ReadFailed, Phase::FileIO, Operation::Read,
                             QString(1000, QChar('x')), 42, 7);
    EXPECT_EQ(e.detail.size(), 256); EXPECT_EQ(e.offset, 42); EXPECT_EQ(e.deviceCode, 7);
}

TEST(CacheArithmetic, OverflowAndBoundaries)
{
    EXPECT_EQ(checked::add(UINT64_MAX, 0), UINT64_MAX);
    EXPECT_FALSE(checked::add(UINT64_MAX, 1));
    EXPECT_EQ(checked::multiply(UINT64_MAX, 1), UINT64_MAX);
    EXPECT_EQ(checked::multiply(UINT64_MAX, 0), 0);
    EXPECT_FALSE(checked::multiply(UINT64_MAX / 24 + 1, 24));
    EXPECT_EQ(checked::pageCount(UINT64_MAX, 1), UINT64_MAX);
    EXPECT_EQ(checked::pageCount(UINT64_MAX, 2), UINT64_MAX / 2 + 1);
    EXPECT_EQ(checked::pageCount(0, 2726), 0);
    EXPECT_EQ(checked::pageCount(2726, 2726), 1);
    EXPECT_EQ(checked::pageCount(2727, 2726), 2);
    EXPECT_FALSE(checked::pageCount(1, 0));
}

TEST(CacheArithmetic, RangesAndConversions)
{
    EXPECT_TRUE(checked::range(100, 0, 100));
    EXPECT_TRUE(checked::range(0, UINT64_MAX, UINT64_MAX));
    EXPECT_FALSE(checked::range(101, 0, 100));
    EXPECT_FALSE(checked::range(UINT64_MAX, 1, UINT64_MAX));
    EXPECT_FALSE(checked::range(UINT64_MAX - 5, 10, UINT64_MAX));
    EXPECT_EQ(checked::fileOffset(INT64_MAX), INT64_MAX);
    EXPECT_FALSE(checked::fileOffset(static_cast<std::uint64_t>(INT64_MAX) + 1));
    EXPECT_FALSE(checked::fileOffset(UINT64_MAX));
    EXPECT_EQ(checked::memorySize(SIZE_MAX), SIZE_MAX);
    // uint64_t and size_t have the same range on the required Windows x64 target.
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
        EXPECT_FALSE(checked::memorySize(UINT64_MAX));
    else
        EXPECT_EQ(checked::memorySize(UINT64_MAX), UINT64_MAX);
}

TEST(CacheArithmetic, LayoutRejectsOverflowAndQtOffsetOverflow)
{
    const auto empty = calculateLayout(0, 0);
    ASSERT_TRUE(empty); EXPECT_EQ(empty.value().fileSize, 8192);
    const auto layout = calculateLayout(2727, 3);
    ASSERT_TRUE(layout);
    EXPECT_EQ(layout.value().pointPages, 2);
    EXPECT_EQ(layout.value().indexOffset, 135168);
    EXPECT_EQ(layout.value().footerOffset, 331776);
    EXPECT_EQ(layout.value().fileSize, 335872);
    EXPECT_FALSE(calculateLayout(UINT64_MAX, 0));
    EXPECT_FALSE(calculateLayout(1, UINT64_MAX));
    EXPECT_FALSE(calculateLayout(0, static_cast<std::uint64_t>(INT64_MAX) / 65536 + 1));
    const auto maximum = calculateLayout(0, (static_cast<std::uint64_t>(INT64_MAX) - 8192) / 65536);
    EXPECT_TRUE(maximum);
}
