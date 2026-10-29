#include "raster_mapping.h"

#include <cfenv>
#include <cmath>
#include <limits>
#include <xmmintrin.h>

// This translation unit is also compiled with /fp:strict. Keep contraction explicitly
// disabled at the definition, including when compiler/target defaults change.
#pragma fenv_access(on)
#pragma fp_contract(off)

namespace cadcontour::raster {
namespace {

static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559
              && std::numeric_limits<double>::radix == 2
              && std::numeric_limits<double>::digits == 53);
static_assert(maxBoundaryIndex <= std::numeric_limits<Index>::max() / 2);

bool supportedEnvironment() noexcept
{
    // x64 MSVC scalar binary64 uses SSE2. Ignore sticky status bits, but require
    // all exception masks, RN-even, and neither flush-to-zero nor denormals-are-zero.
    return (_mm_getcsr() & 0xffc0u) == 0x1f80u && std::fegetround() == FE_TONEAREST;
}

double boundary(double origin, double cell, Index index) noexcept
{
    if (index == 0)
        return origin;
    const double offset = static_cast<double>(index) * cell;
    return origin + offset;
}

Result<Index> dimension(double origin, double maximum, double cell, Axis axis) noexcept
{
    if (maximum == origin)
        return Index{1};

    // B(i) is nondecreasing (positive cell, exact integer conversion, RN-even).
    // Overflow is an upper search sentinel, never an accepted terminal boundary.
    Index first = 1;
    Index last = maxBoundaryIndex;
    while (first < last) {
        const Index mid = first + (last - first) / 2;
        const double value = boundary(origin, cell, mid);
        if (!std::isfinite(value) || value >= maximum)
            last = mid;
        else
            first = mid + 1;
    }
    const double terminal = boundary(origin, cell, first);
    if (!std::isfinite(terminal))
        return Error{ErrorCode::BoundaryOverflow, axis, first};
    if (terminal < maximum)
        return Error{ErrorCode::DimensionUnrepresentable, axis, first};
    return first;
}

std::optional<Error> validateAxis(double origin, double cell, Index size, Axis axis,
                                  Mapping::CancelCheck cancel, void *context) noexcept
{
    double previous = origin;
    for (Index i = 1; i <= size; ++i) {
        if ((i - 1) % 4096 == 0) {
            if (cancel && cancel(context))
                return Error{ErrorCode::Cancelled, axis, i};
            // A callback may have changed the caller's thread-local FP environment.
            if (!supportedEnvironment())
                return Error{ErrorCode::UnsupportedEnvironment, axis, i};
        }
        const double current = boundary(origin, cell, i);
        if (!std::isfinite(current))
            return Error{ErrorCode::BoundaryOverflow, axis, i};
        if (current <= previous)
            return Error{ErrorCode::CollapsedBoundary, axis, i};
        previous = current;
    }
    return std::nullopt;
}

Index locateAxis(double origin, double maximum, double cell, Index size, double point) noexcept
{
    if (point == maximum)
        return size - 1;
    Index first = 0;
    Index last = size;
    while (first + 1 < last) {
        const Index mid = first + (last - first) / 2;
        if (point < boundary(origin, cell, mid))
            last = mid;
        else
            first = mid;
    }
    return first;
}

AxisCellBounds axisCellBounds(double origin, double maximum, double cell,
                              Index size, Index index) noexcept
{
    const double lower = boundary(origin, cell, index);
    const double upper = boundary(origin, cell, index + 1);
    const bool final = index == size - 1;
    return {{lower, upper, true, final}, {lower, final ? maximum : upper, true, final}};
}

} // namespace

ErrorCategory Error::category() const noexcept
{
    switch (code) {
    case ErrorCode::ResourceRefused: return ErrorCategory::ResourceRefused;
    case ErrorCode::Cancelled: return ErrorCategory::Cancelled;
    case ErrorCode::UnsupportedEnvironment:
    case ErrorCode::DimensionUnrepresentable:
    case ErrorCode::BoundaryOverflow:
    case ErrorCode::CollapsedBoundary: return ErrorCategory::Numerical;
    default: return ErrorCategory::InvalidInput;
    }
}

Mapping::Mapping(ModelPoint minimum, ModelPoint maximum, double cell, Index width, Index height) noexcept
    : minimum_(minimum), maximum_(maximum), cell_(cell), width_(width), height_(height)
{}

Result<Mapping> Mapping::create(ModelPoint minimum, ModelPoint maximum, double cell,
                                ValidationPolicy policy, CancelCheck cancel, void *context) noexcept
{
    if (cancel && cancel(context))
        return Error{ErrorCode::Cancelled, std::nullopt, std::nullopt};
    if (!supportedEnvironment())
        return Error{ErrorCode::UnsupportedEnvironment, std::nullopt, std::nullopt};
    if (!std::isfinite(cell) || cell <= 0.0)
        return Error{ErrorCode::InvalidCell, std::nullopt, std::nullopt};
    if (!std::isfinite(minimum.x) || !std::isfinite(maximum.x) || maximum.x < minimum.x)
        return Error{ErrorCode::InvalidBounds, Axis::X, std::nullopt};
    if (!std::isfinite(minimum.y) || !std::isfinite(maximum.y) || maximum.y < minimum.y)
        return Error{ErrorCode::InvalidBounds, Axis::Y, std::nullopt};

    const auto width = dimension(minimum.x, maximum.x, cell, Axis::X);
    if (!width)
        return width.error();
    const auto height = dimension(minimum.y, maximum.y, cell, Axis::Y);
    if (!height)
        return height.error();
    // Both dimensions <= 2^53: their sum <= 2^54 cannot overflow Index.
    // No width*height, linear raster ordinal, or storage offset is needed here.
    const Index checks = width.value() + height.value();
    if (cancel && cancel(context))
        return Error{ErrorCode::Cancelled, std::nullopt, std::nullopt, checks};
    if (checks > policy.maxBoundaryChecks)
        return Error{ErrorCode::ResourceRefused, std::nullopt, std::nullopt, checks};

    if (const auto error = validateAxis(minimum.x, cell, width.value(), Axis::X, cancel, context))
        return *error;
    if (const auto error = validateAxis(minimum.y, cell, height.value(), Axis::Y, cancel, context))
        return *error;
    if (cancel && cancel(context))
        return Error{ErrorCode::Cancelled, std::nullopt, std::nullopt, checks};
    if (!supportedEnvironment())
        return Error{ErrorCode::UnsupportedEnvironment, std::nullopt, std::nullopt};
    return Mapping(minimum, maximum, cell, width.value(), height.value());
}

Result<CellIndex> Mapping::locate(ModelPoint point) const noexcept
{
    if (!supportedEnvironment())
        return Error{ErrorCode::UnsupportedEnvironment, std::nullopt, std::nullopt};
    if (!std::isfinite(point.x) || !std::isfinite(point.y))
        return Error{ErrorCode::InvalidPoint, std::nullopt, std::nullopt};
    if (point.x < minimum_.x || point.x > maximum_.x)
        return Error{ErrorCode::PointOutsideBounds, Axis::X, std::nullopt};
    if (point.y < minimum_.y || point.y > maximum_.y)
        return Error{ErrorCode::PointOutsideBounds, Axis::Y, std::nullopt};
    return CellIndex{locateAxis(minimum_.x, maximum_.x, cell_, width_, point.x),
                     locateAxis(minimum_.y, maximum_.y, cell_, height_, point.y)};
}

Result<CellBounds> Mapping::cellBounds(CellIndex index) const noexcept
{
    if (!supportedEnvironment())
        return Error{ErrorCode::UnsupportedEnvironment, std::nullopt, std::nullopt};
    if (index.x >= width_)
        return Error{ErrorCode::IndexOutsideBounds, Axis::X, index.x};
    if (index.y >= height_)
        return Error{ErrorCode::IndexOutsideBounds, Axis::Y, index.y};
    return CellBounds{axisCellBounds(minimum_.x, maximum_.x, cell_, width_, index.x),
                      axisCellBounds(minimum_.y, maximum_.y, cell_, height_, index.y)};
}

} // namespace cadcontour::raster
