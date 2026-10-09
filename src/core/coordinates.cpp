#include "coordinates.h"

#include <cmath>

namespace cadcontour {
namespace {

bool finite(double x, double y) noexcept { return std::isfinite(x) && std::isfinite(y); }

bool valid(const ModelViewTransform &transform) noexcept
{
    return finite(transform.modelAnchor.x, transform.modelAnchor.y)
        && finite(transform.viewAnchor.x, transform.viewAnchor.y)
        && std::isfinite(transform.pixelsPerMm) && transform.pixelsPerMm > 0.0;
}

} // namespace

CoordinateResult<ViewPoint> ModelViewTransform::toView(ModelPoint point) const noexcept
{
    if (!valid(*this))
        return {std::nullopt, CoordinateError::InvalidTransform};
    if (!finite(point.x, point.y))
        return {std::nullopt, CoordinateError::InvalidPoint};
    const double dx = point.x - modelAnchor.x;
    const double dy = modelAnchor.y - point.y;
    const double px = dx * pixelsPerMm;
    const double py = dy * pixelsPerMm;
    const ViewPoint result{viewAnchor.x + px, viewAnchor.y + py};
    if (!finite(dx, dy) || !finite(px, py) || !finite(result.x, result.y))
        return {std::nullopt, CoordinateError::ArithmeticOverflow};
    return {result, std::nullopt};
}

CoordinateResult<ModelPoint> ModelViewTransform::toModel(ViewPoint point) const noexcept
{
    if (!valid(*this))
        return {std::nullopt, CoordinateError::InvalidTransform};
    if (!finite(point.x, point.y))
        return {std::nullopt, CoordinateError::InvalidPoint};
    const double dx = point.x - viewAnchor.x;
    const double dy = viewAnchor.y - point.y;
    const double mx = dx / pixelsPerMm;
    const double my = dy / pixelsPerMm;
    const ModelPoint result{modelAnchor.x + mx, modelAnchor.y + my};
    if (!finite(dx, dy) || !finite(mx, my) || !finite(result.x, result.y))
        return {std::nullopt, CoordinateError::ArithmeticOverflow};
    return {result, std::nullopt};
}

} // namespace cadcontour
