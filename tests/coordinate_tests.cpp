#include "core/coordinates.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace cadcontour;

TEST(Coordinates, TranslationScaleAndInvertedY)
{
    const ModelViewTransform transform{{10, 20}, {100, 200}, 4};
    const auto view = transform.toView({13, 25});
    ASSERT_TRUE(view.point);
    EXPECT_FALSE(view.error);
    EXPECT_DOUBLE_EQ(view.point->x, 112);
    EXPECT_DOUBLE_EQ(view.point->y, 180);
    const auto model = transform.toModel(*view.point);
    ASSERT_TRUE(model.point);
    EXPECT_DOUBLE_EQ(model.point->x, 13);
    EXPECT_DOUBLE_EQ(model.point->y, 25);
    const auto anchor = transform.toView(transform.modelAnchor);
    ASSERT_TRUE(anchor.point);
    EXPECT_DOUBLE_EQ(anchor.point->x, 100);
    EXPECT_DOUBLE_EQ(anchor.point->y, 200);
}

TEST(Coordinates, NegativeCoordinatesAndIdentityScale)
{
    ModelViewTransform transform;
    auto view = transform.toView({-12.5, -7.25});
    ASSERT_TRUE(view.point);
    EXPECT_DOUBLE_EQ(view.point->x, -12.5);
    EXPECT_DOUBLE_EQ(view.point->y, 7.25);
    auto model = transform.toModel(*view.point);
    ASSERT_TRUE(model.point);
    EXPECT_DOUBLE_EQ(model.point->x, -12.5);
    EXPECT_DOUBLE_EQ(model.point->y, -7.25);
}

TEST(Coordinates, LargeLocalAnchorAvoidsHugeViewCoordinates)
{
    const ModelViewTransform transform{{1e15, -1e15}, {320, 240}, 8};
    const ModelPoint point{1e15 + 2.25, -1e15 - 3.5};
    const auto view = transform.toView(point);
    ASSERT_TRUE(view.point);
    EXPECT_DOUBLE_EQ(view.point->x, 338);
    EXPECT_DOUBLE_EQ(view.point->y, 268);
    const auto model = transform.toModel(*view.point);
    ASSERT_TRUE(model.point);
    EXPECT_DOUBLE_EQ(model.point->x, point.x);
    EXPECT_DOUBLE_EQ(model.point->y, point.y);
}

TEST(Coordinates, RoundTripToleranceTracksEachArithmeticScale)
{
    for (double anchor : {0.0, -1234.5, 1e9, -1e15}) {
        for (double scale : {0.03125, 0.7, 1.0, 13.3, 1e6}) {
            const ModelViewTransform transform{{anchor, -anchor}, {320.5, -240.25}, scale};
            const ModelPoint original{anchor + 7.25, -anchor - 18.125};
            const auto view = transform.toView(original);
            ASSERT_TRUE(view.point);
            const auto restored = transform.toModel(*view.point);
            ASSERT_TRUE(restored.point);
            // Bound in model units: model magnitude plus view-addition rounding / scale.
            const double eps = std::numeric_limits<double>::epsilon();
            const double tx = 8 * eps * (std::abs(original.x) + std::abs(anchor)
                              + (std::abs(view.point->x) + 320.5) / scale);
            const double ty = 8 * eps * (std::abs(original.y) + std::abs(anchor)
                              + (std::abs(view.point->y) + 240.25) / scale);
            EXPECT_NEAR(restored.point->x, original.x, tx);
            EXPECT_NEAR(restored.point->y, original.y, ty);
            const auto viewAgain = transform.toView(*restored.point);
            ASSERT_TRUE(viewAgain.point);
            EXPECT_NEAR(viewAgain.point->x, view.point->x, tx * scale);
            EXPECT_NEAR(viewAgain.point->y, view.point->y, ty * scale);
        }
    }
}

TEST(Coordinates, RejectsInvalidScalesAndAnchors)
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (double scale : {0.0, -0.0, -1.0, inf, -inf, nan}) {
        ModelViewTransform transform{{}, {}, scale};
        EXPECT_EQ(transform.toView({}).error, CoordinateError::InvalidTransform);
        EXPECT_EQ(transform.toModel({}).error, CoordinateError::InvalidTransform);
    }
    for (double value : {inf, -inf, nan}) {
        for (int field = 0; field < 4; ++field) {
            ModelViewTransform transform;
            switch (field) {
            case 0: transform.modelAnchor.x = value; break;
            case 1: transform.modelAnchor.y = value; break;
            case 2: transform.viewAnchor.x = value; break;
            case 3: transform.viewAnchor.y = value; break;
            }
            EXPECT_EQ(transform.toView({}).error, CoordinateError::InvalidTransform);
            EXPECT_EQ(transform.toModel({}).error, CoordinateError::InvalidTransform);
        }
    }
}

TEST(Coordinates, RejectsNonFinitePoints)
{
    ModelViewTransform transform;
    for (double value : {std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_EQ(transform.toView({value, 0}).error, CoordinateError::InvalidPoint);
        EXPECT_EQ(transform.toView({0, value}).error, CoordinateError::InvalidPoint);
        EXPECT_EQ(transform.toModel({value, 0}).error, CoordinateError::InvalidPoint);
        EXPECT_EQ(transform.toModel({0, value}).error, CoordinateError::InvalidPoint);
    }
}

TEST(Coordinates, RejectsOverflowInSubtractionScalingAndTranslation)
{
    const double max = std::numeric_limits<double>::max();
    const ModelViewTransform subtraction{{-max, max}, {-max, max}, 1};
    EXPECT_EQ(subtraction.toView({max, -max}).error, CoordinateError::ArithmeticOverflow);
    EXPECT_EQ(subtraction.toModel({max, -max}).error, CoordinateError::ArithmeticOverflow);
    const ModelViewTransform multiplication{{}, {}, max};
    EXPECT_EQ(multiplication.toView({2, 0}).error, CoordinateError::ArithmeticOverflow);
    EXPECT_EQ(multiplication.toView({0, 2}).error, CoordinateError::ArithmeticOverflow);
    const ModelViewTransform division{{}, {}, std::numeric_limits<double>::min()};
    EXPECT_EQ(division.toModel({max, 0}).error, CoordinateError::ArithmeticOverflow);
    EXPECT_EQ(division.toModel({0, max}).error, CoordinateError::ArithmeticOverflow);
    const ModelViewTransform translation{{max, max}, {max, max}, 1};
    EXPECT_EQ(translation.toView({max, -max}).error, CoordinateError::ArithmeticOverflow);
    EXPECT_EQ(translation.toModel({max, -max}).error, CoordinateError::ArithmeticOverflow);
    const ModelViewTransform viewAddition{{}, {max, max}, 1};
    EXPECT_EQ(viewAddition.toView({max, -max}).error, CoordinateError::ArithmeticOverflow);
    const ModelViewTransform modelAddition{{max, max}, {}, 1};
    EXPECT_EQ(modelAddition.toModel({max, -max}).error, CoordinateError::ArithmeticOverflow);
}

TEST(Coordinates, AcceptsTinyPositiveScaleWhenResultsAreRepresentable)
{
    const double tiny = std::numeric_limits<double>::denorm_min();
    const ModelViewTransform transform{{}, {}, tiny};
    const auto view = transform.toView({1, 1});
    ASSERT_TRUE(view.point);
    EXPECT_EQ(view.point->x, tiny);
    EXPECT_EQ(view.point->y, -tiny);
    const auto model = transform.toModel(*view.point);
    ASSERT_TRUE(model.point);
    EXPECT_DOUBLE_EQ(model.point->x, 1);
    EXPECT_DOUBLE_EQ(model.point->y, 1);
}
