#include "core/raster_mapping.h"

#include <gtest/gtest.h>

#include <bit>
#include <cfenv>
#include <cmath>
#include <limits>
#include <type_traits>
#include <vector>
#include <xmmintrin.h>

using cadcontour::ModelPoint;
using namespace cadcontour::raster;

namespace {

constexpr ValidationPolicy policy{initialBoundaryCheckBudget};
constexpr double inf = std::numeric_limits<double>::infinity();
constexpr double tiny = std::numeric_limits<double>::denorm_min();
constexpr double largest = std::numeric_limits<double>::max();
double number(std::uint64_t bits) { return std::bit_cast<double>(bits); }
std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

struct Golden {
    const char *name;
    std::uint64_t origin;
    std::uint64_t cell;
    std::uint64_t maximum;
    std::vector<std::uint64_t> boundaries;
};

const Golden golden[] = {
#include "fixtures/raster/raster_boundaries.inc"
};

bool contains(const Interval &interval, double point)
{
    return (point > interval.minimum || (interval.minimumIncluded && point == interval.minimum))
        && (point < interval.maximum || (interval.maximumIncluded && point == interval.maximum));
}

struct RestoreEnvironment {
    unsigned int csr = _mm_getcsr();
    std::fenv_t environment{};
    RestoreEnvironment() { std::fegetenv(&environment); }
    ~RestoreEnvironment() { std::fesetenv(&environment); _mm_setcsr(csr); }
};

struct Cancellation {
    unsigned calls = 0;
    unsigned at;
    static bool check(void *context) noexcept
    {
        auto &self = *static_cast<Cancellation *>(context);
        return ++self.calls == self.at;
    }
};

} // namespace

TEST(RasterMapping, ImmutableValidatedValueAndMetadata)
{
    static_assert(!std::is_default_constructible_v<Mapping>);
    static_assert(!std::is_default_constructible_v<ValidationPolicy>);
    const auto result = Mapping::create({-4, -3}, {2, 7}, 1, policy);
    ASSERT_TRUE(result);
    const auto &mapping = result.value();
    EXPECT_EQ(mapping.origin().x, -4);
    EXPECT_EQ(mapping.origin().y, -3);
    EXPECT_EQ(mapping.minimum().x, -4);
    EXPECT_EQ(mapping.maximum().y, 7);
    EXPECT_EQ(mapping.cell(), 1);
    EXPECT_EQ(mapping.width(), 6);
    EXPECT_EQ(mapping.height(), 10);
    EXPECT_EQ(mapping.convention(), Convention::Binary64SeparateRoundNearestEven);
}

TEST(RasterMapping, GoldenBitsDimensionsMinimalityAndUniqueOwnership)
{
    for (const auto &fixture : golden) {
        SCOPED_TRACE(fixture.name);
        const double origin = number(fixture.origin);
        const double maximum = number(fixture.maximum);
        const auto created = Mapping::create({origin, origin}, {maximum, maximum},
                                              number(fixture.cell), policy);
        ASSERT_TRUE(created);
        const auto &mapping = created.value();
        const Index size = fixture.boundaries.size() - 1;
        ASSERT_EQ(mapping.width(), size);
        ASSERT_EQ(mapping.height(), size);
        EXPECT_LT(number(fixture.boundaries[size - 1]), maximum);
        EXPECT_GE(number(fixture.boundaries[size]), maximum);
        for (Index i = 0; i < size; ++i) {
            const auto bounds = mapping.cellBounds({i, i});
            ASSERT_TRUE(bounds);
            for (const auto &axis : {bounds.value().x, bounds.value().y}) {
                EXPECT_EQ(bits(axis.nominal.minimum), fixture.boundaries[i]);
                EXPECT_EQ(bits(axis.nominal.maximum), fixture.boundaries[i + 1]);
                EXPECT_TRUE(axis.nominal.minimumIncluded);
                EXPECT_EQ(axis.nominal.maximumIncluded, i + 1 == size);
                EXPECT_EQ(bits(axis.coverage.minimum), fixture.boundaries[i]);
                EXPECT_EQ(bits(axis.coverage.maximum), i + 1 == size ? fixture.maximum : fixture.boundaries[i + 1]);
            }
        }
        std::vector<double> points{origin, maximum, std::nextafter(origin, -inf),
                                   std::nextafter(maximum, inf)};
        for (const auto boundaryBits : fixture.boundaries) {
            const double boundary = number(boundaryBits);
            points.insert(points.end(), {std::nextafter(boundary, -inf), boundary,
                                          std::nextafter(boundary, inf)});
        }
        for (const double point : points) {
            const auto located = mapping.locate({point, point});
            if (point < origin || point > maximum) {
                ASSERT_FALSE(located);
                EXPECT_EQ(located.error().code, ErrorCode::PointOutsideBounds);
                continue;
            }
            // Independent linear ownership over golden intervals, no quotient or
            // production boundary helper. Check every cell for unique membership.
            Index expected = size - 1;
            if (point != maximum) {
                expected = 0;
                while (expected + 1 < size && point >= number(fixture.boundaries[expected + 1]))
                    ++expected;
            }
            ASSERT_TRUE(located);
            EXPECT_EQ(located.value(), (CellIndex{expected, expected}));
            unsigned owners = 0;
            for (Index i = 0; i < size; ++i) {
                const auto bounds = mapping.cellBounds({i, i});
                ASSERT_TRUE(bounds);
                const bool owns = contains(bounds.value().x.coverage, point)
                               && contains(bounds.value().y.coverage, point);
                if (owns) {
                    ++owners;
                    EXPECT_EQ(i, expected);
                }
            }
            EXPECT_EQ(owners, 1u);
        }
    }
}

TEST(RasterMapping, TerminalCellBinary64PointOneRegression)
{
    // Exact binary64(3 * binary64(0.1)), distinct from binary64(decimal 0.3).
    const double maximum = number(0x3fd3333333333334ULL);
    const auto result = Mapping::create({0, 0}, {maximum, maximum}, 0.1, policy);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().width(), 3);
    EXPECT_EQ(result.value().height(), 3);
    EXPECT_EQ(bits(maximum), bits(3 * result.value().cell()));
    EXPECT_NE(bits(maximum), bits(0.3));
    const auto located = result.value().locate({maximum, maximum});
    ASSERT_TRUE(located);
    EXPECT_EQ(located.value(), (CellIndex{2, 2}));
}

TEST(RasterMapping, SeparateRoundingIsObservablyDifferentFromFusedArithmetic)
{
    const auto created = Mapping::create({-1, -1}, {0.1, 0.1}, 0.1, policy);
    ASSERT_TRUE(created);
    const auto bounds = created.value().cellBounds({9, 9});
    ASSERT_TRUE(bounds);
    EXPECT_EQ(bits(bounds.value().x.nominal.maximum), 0u);
    // Exact 10 * binary64(0.1) - 1 = 2^-54 before rounding the product.
    EXPECT_EQ(bits(std::fma(10., 0.1, -1.)), 0x3c90000000000000ULL);
}

TEST(RasterMapping, PartialEdgesAndMixedAxisIndices)
{
    const auto created = Mapping::create({-2, -1}, {0.5, 1.25}, 1, policy);
    ASSERT_TRUE(created);
    const auto &mapping = created.value();
    ASSERT_EQ(mapping.width(), 3);
    ASSERT_EQ(mapping.height(), 3);
    auto located = mapping.locate({-1, 1.25});
    ASSERT_TRUE(located);
    EXPECT_EQ(located.value(), (CellIndex{1, 2}));
    const auto bounds = mapping.cellBounds({2, 2});
    ASSERT_TRUE(bounds);
    EXPECT_EQ(bounds.value().x.nominal.minimum, 0);
    EXPECT_EQ(bounds.value().x.nominal.maximum, 1);
    EXPECT_EQ(bounds.value().x.coverage.maximum, 0.5);
    EXPECT_EQ(bounds.value().y.nominal.maximum, 2);
    EXPECT_EQ(bounds.value().y.coverage.maximum, 1.25);
    EXPECT_TRUE(bounds.value().x.coverage.maximumIncluded);
    EXPECT_TRUE(bounds.value().y.coverage.maximumIncluded);
}

TEST(RasterMapping, DegenerateXandYAndBothAxes)
{
    for (const auto maximum : {ModelPoint{4, 9}, ModelPoint{7, 5}, ModelPoint{4, 5}}) {
        const auto created = Mapping::create({4, 5}, maximum, 1, policy);
        ASSERT_TRUE(created);
        const auto &mapping = created.value();
        EXPECT_EQ(mapping.width(), maximum.x == 4 ? 1 : 3);
        EXPECT_EQ(mapping.height(), maximum.y == 5 ? 1 : 4);
        const auto bounds = mapping.cellBounds({0, 0});
        ASSERT_TRUE(bounds);
        if (maximum.x == 4) {
            EXPECT_EQ(bounds.value().x.coverage.minimum, 4);
            EXPECT_EQ(bounds.value().x.coverage.maximum, 4);
            EXPECT_EQ(bounds.value().x.nominal.maximum, 5);
            EXPECT_TRUE(bounds.value().x.coverage.maximumIncluded);
        }
        if (maximum.y == 5) {
            EXPECT_EQ(bounds.value().y.coverage.minimum, 5);
            EXPECT_EQ(bounds.value().y.coverage.maximum, 5);
            EXPECT_EQ(bounds.value().y.nominal.maximum, 6);
            EXPECT_TRUE(bounds.value().y.coverage.maximumIncluded);
        }
        ASSERT_TRUE(mapping.locate(maximum));
        EXPECT_FALSE(mapping.locate({std::nextafter(4., -inf), 5}));
        EXPECT_FALSE(mapping.locate({maximum.x, std::nextafter(maximum.y, inf)}));
    }
}

TEST(RasterMapping, SignedZerosHaveEqualGeometricOwnershipAndPreservedMetadata)
{
    for (double minimum : {0.0, -0.0}) {
        for (double maximum : {0.0, -0.0, 1.0}) {
            const auto created = Mapping::create({minimum, minimum}, {maximum, maximum}, 1, policy);
            ASSERT_TRUE(created);
            EXPECT_EQ(bits(created.value().origin().x), bits(minimum));
            EXPECT_EQ(bits(created.value().maximum().y), bits(maximum));
            for (double point : {0.0, -0.0}) {
                const auto located = created.value().locate({point, point});
                ASSERT_TRUE(located);
                EXPECT_EQ(located.value(), (CellIndex{0, 0}));
            }
        }
    }
}

TEST(RasterMapping, RejectsInvalidBoundsAndCell)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (double cell : {0., -0., -1., inf, -inf, nan}) {
        const auto result = Mapping::create({}, {1, 1}, cell, policy);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::InvalidCell);
        EXPECT_EQ(result.error().category(), ErrorCategory::InvalidInput);
    }
    for (double invalid : {inf, -inf, nan}) {
        for (int component = 0; component < 4; ++component) {
            ModelPoint minimum{};
            ModelPoint maximum{1, 1};
            switch (component) {
            case 0: minimum.x = invalid; break;
            case 1: minimum.y = invalid; break;
            case 2: maximum.x = invalid; break;
            case 3: maximum.y = invalid; break;
            }
            const auto result = Mapping::create(minimum, maximum, 1, policy);
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, ErrorCode::InvalidBounds);
        }
    }
    EXPECT_FALSE(Mapping::create({2, 0}, {1, 1}, 1, policy));
    EXPECT_FALSE(Mapping::create({0, 2}, {1, 1}, 1, policy));
}

TEST(RasterMapping, InvalidPointsOutsideBoundsAndIndices)
{
    const auto created = Mapping::create({-1, -1}, {1, 1}, 1, policy);
    ASSERT_TRUE(created);
    const auto &mapping = created.value();
    for (double invalid : {inf, -inf, std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(mapping.locate({invalid, 0}).error().code, ErrorCode::InvalidPoint);
        EXPECT_EQ(mapping.locate({0, invalid}).error().code, ErrorCode::InvalidPoint);
    }
    for (double outside : {std::nextafter(-1., -inf), std::nextafter(1., inf)}) {
        EXPECT_EQ(mapping.locate({outside, 0}).error().code, ErrorCode::PointOutsideBounds);
        EXPECT_EQ(mapping.locate({0, outside}).error().code, ErrorCode::PointOutsideBounds);
    }
    for (Index index : {Index{2}, maxBoundaryIndex, std::numeric_limits<Index>::max()}) {
        EXPECT_EQ(mapping.cellBounds({index, 0}).error().code, ErrorCode::IndexOutsideBounds);
        EXPECT_EQ(mapping.cellBounds({0, index}).error().code, ErrorCode::IndexOutsideBounds);
    }
}

TEST(RasterMapping, SafeNumericalRefusalsAndOverflowSearchSentinels)
{
    const auto dimension = Mapping::create({}, {1, 1}, tiny, policy);
    ASSERT_FALSE(dimension);
    EXPECT_EQ(dimension.error().code, ErrorCode::DimensionUnrepresentable);
    EXPECT_EQ(dimension.error().category(), ErrorCategory::Numerical);
    EXPECT_EQ(dimension.error().boundaryIndex, maxBoundaryIndex);

    // Search probes can overflow, although the required final boundary is finite.
    const auto finite = Mapping::create({}, {largest, largest}, largest, policy);
    ASSERT_TRUE(finite);
    EXPECT_EQ(finite.value().width(), 1);
    EXPECT_TRUE(finite.value().locate({largest, largest}));
    // No subtract/divide estimate is needed even at the largest coordinate scale.
    const auto hugeSpan = Mapping::create({-largest, 0}, {-1, 0}, largest, policy);
    ASSERT_TRUE(hugeSpan);
    EXPECT_EQ(hugeSpan.value().width(), 1);
    // Here the required product overflows even though a fused operation could be finite.
    const auto overflow = Mapping::create({-largest, 0}, {largest, 0}, largest, policy);
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, ErrorCode::BoundaryOverflow);
    const auto degenerateOverflow = Mapping::create({largest, 0}, {largest, 0}, largest, policy);
    ASSERT_FALSE(degenerateOverflow);
    EXPECT_EQ(degenerateOverflow.error().code, ErrorCode::BoundaryOverflow);

    const auto collapsed = Mapping::create({1, 0}, {1, 0}, tiny, policy);
    ASSERT_FALSE(collapsed);
    EXPECT_EQ(collapsed.error().code, ErrorCode::CollapsedBoundary);
    EXPECT_EQ(collapsed.error().boundaryIndex, 1);
    // A later collapsed interval: B(0)..B(3) increase, B(4) == B(3).
    const auto laterCollapse = Mapping::create({number(0x433ffffffffffffdULL), 0},
                                               {number(0x4340000000000002ULL), 0}, 1, policy);
    ASSERT_FALSE(laterCollapse);
    EXPECT_EQ(laterCollapse.error().code, ErrorCode::CollapsedBoundary);
    EXPECT_EQ(laterCollapse.error().boundaryIndex, 4);
}

TEST(RasterMapping, BudgetCheckedBeforeTraversalWithoutAreaProduct)
{
    const auto refused = Mapping::create({}, {3, 4}, 1, ValidationPolicy{6});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().category(), ErrorCategory::ResourceRefused);
    EXPECT_EQ(refused.error().requiredBoundaryChecks, 7);
    EXPECT_TRUE(Mapping::create({}, {3, 4}, 1, ValidationPolicy{7}));
    EXPECT_EQ(Mapping::create({}, {}, 1, ValidationPolicy{0}).error().code, ErrorCode::ResourceRefused);
    // width*height exceeds uint64; foundation only needs the checked axis sum.
    const auto huge = Mapping::create({}, {0x1p53, 0x1p53}, 1, ValidationPolicy{10});
    ASSERT_FALSE(huge);
    EXPECT_EQ(huge.error().code, ErrorCode::ResourceRefused);
    EXPECT_EQ(huge.error().requiredBoundaryChecks, Index{1} << 54);
    // The first binary64 coordinate above 2^53 requires an unsupported dimension.
    const auto tooWide = Mapping::create({}, {0x1.0000000000001p53, 0}, 1, policy);
    ASSERT_FALSE(tooWide);
    EXPECT_EQ(tooWide.error().code, ErrorCode::DimensionUnrepresentable);
}

TEST(RasterMapping, CancellationBeforeAndDuringEachAxisAndBeforeSuccess)
{
    // Calls: entry, preflight, X at 1/4097/8193, Y at 1/4097/8193, completion.
    for (unsigned at : {1u, 2u, 3u, 4u, 6u, 7u, 9u}) {
        Cancellation cancellation{0, at};
        const auto result = Mapping::create({}, {10000, 10000}, 1, policy,
                                              Cancellation::check, &cancellation);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().category(), ErrorCategory::Cancelled);
        EXPECT_EQ(cancellation.calls, at);
        if (at == 4 || at == 7) {
            EXPECT_EQ(result.error().boundaryIndex, 4097);
            EXPECT_EQ(result.error().axis, at == 4 ? Axis::X : Axis::Y);
        }
    }
}

TEST(RasterMapping, RejectsRoundingFTZDAZAndUnmaskedExceptionsWithoutChangingEnvironment)
{
    RestoreEnvironment restore;
    const auto created = Mapping::create({}, {1, 1}, 0.1, policy);
    ASSERT_TRUE(created);
    const unsigned original = _mm_getcsr();
    for (unsigned change : {0x2000u, 0x4000u, 0x6000u, 0x8000u, 0x40u}) {
        const unsigned changed = (original & ~0x6000u) | change;
        _mm_setcsr(changed);
        EXPECT_EQ(Mapping::create({}, {1, 1}, 0.1, policy).error().code, ErrorCode::UnsupportedEnvironment);
        EXPECT_EQ(created.value().locate({0.1, 0.1}).error().code, ErrorCode::UnsupportedEnvironment);
        EXPECT_EQ(created.value().cellBounds({0, 0}).error().code, ErrorCode::UnsupportedEnvironment);
        EXPECT_EQ(_mm_getcsr(), changed);
    }
    for (unsigned mask : {0x80u, 0x100u, 0x200u, 0x400u, 0x800u, 0x1000u}) {
        // Clear sticky flags before unmasking; no FP arithmetic until it is restored.
        const unsigned changed = (original & ~0x3fu) & ~mask;
        _mm_setcsr(changed);
        const auto result = Mapping::create({}, {1, 1}, 0.1, policy);
        _mm_setcsr(original);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::UnsupportedEnvironment);
    }
    _mm_setcsr(original);
    ASSERT_EQ(std::fesetround(FE_DOWNWARD), 0);
    const auto downward = Mapping::create({}, {1, 1}, 1, policy);
    ASSERT_FALSE(downward);
    EXPECT_EQ(downward.error().code, ErrorCode::UnsupportedEnvironment);
}

TEST(RasterMapping, CallbackChangingEnvironmentCannotPublishMapping)
{
    RestoreEnvironment restore;
    struct Callback {
        unsigned calls = 0;
        static bool check(void *context) noexcept {
            auto &self = *static_cast<Callback *>(context);
            if (++self.calls == 4)
                _mm_setcsr(_mm_getcsr() | 0x8000u);
            return false;
        }
    } callback;
    const auto result = Mapping::create({}, {10000, 1}, 1, policy, Callback::check, &callback);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::UnsupportedEnvironment);
}
