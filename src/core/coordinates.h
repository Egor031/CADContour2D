#pragma once

#include <optional>

namespace cadcontour {

struct ModelPoint { double x = 0.0; double y = 0.0; }; // millimetres, Y up
struct ViewPoint { double x = 0.0; double y = 0.0; }; // pixels, Y down

enum class CoordinateError { InvalidPoint, InvalidTransform, ArithmeticOverflow };

template<typename Point>
struct CoordinateResult {
    std::optional<Point> point;
    std::optional<CoordinateError> error;
};

struct ModelViewTransform {
    ModelPoint modelAnchor;
    ViewPoint viewAnchor;
    double pixelsPerMm = 1.0;

    CoordinateResult<ViewPoint> toView(ModelPoint point) const noexcept;
    CoordinateResult<ModelPoint> toModel(ViewPoint point) const noexcept;
};

} // namespace cadcontour
