#pragma once

#include "coordinates.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace cadcontour::raster {

using Index = std::uint64_t;
inline constexpr Index maxBoundaryIndex = Index{1} << 53;
// Initial caller policy, not a dimension/RAM limit. create() requires an explicit policy.
inline constexpr Index initialBoundaryCheckBudget = 10'000'000;

enum class Axis { X, Y };
enum class Convention { Binary64SeparateRoundNearestEven };
enum class ErrorCategory { InvalidInput, Numerical, ResourceRefused, Cancelled };
enum class ErrorCode {
    InvalidBounds, InvalidCell, InvalidPoint, PointOutsideBounds, IndexOutsideBounds,
    UnsupportedEnvironment, DimensionUnrepresentable, BoundaryOverflow, CollapsedBoundary,
    ResourceRefused, Cancelled
};

struct Error {
    ErrorCode code;
    std::optional<Axis> axis;
    std::optional<Index> boundaryIndex;
    Index requiredBoundaryChecks = 0;

    ErrorCategory category() const noexcept;
};

template<class T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(error) {}
    explicit operator bool() const noexcept { return std::holds_alternative<T>(data_); }
    const T &value() const { return std::get<T>(data_); }
    const Error &error() const { return std::get<Error>(data_); }
private:
    std::variant<T, Error> data_;
};

struct ValidationPolicy {
    explicit constexpr ValidationPolicy(Index boundaryChecks) noexcept : maxBoundaryChecks(boundaryChecks) {}
    Index maxBoundaryChecks; // zero refuses every mapping; never means unlimited
};

struct CellIndex {
    Index x = 0;
    Index y = 0;
    bool operator==(const CellIndex &) const = default;
};

struct Interval {
    double minimum;
    double maximum;
    bool minimumIncluded;
    bool maximumIncluded;
};

struct AxisCellBounds {
    // Nominal intervals are half-open, except the final upper boundary is closed.
    Interval nominal;
    // Intersection with the original bounds; a degenerate axis is a closed singleton.
    Interval coverage;
};

struct CellBounds {
    AxisCellBounds x;
    AxisCellBounds y;
};

// Immutable value. No default/partially validated state, allocation, or raster storage.
// All arithmetic methods require RN-even, gradual underflow, masked FP exceptions;
// unsupported caller environments return an error without changing that environment.
class Mapping {
public:
    using CancelCheck = bool (*)(void *) noexcept;
    static Result<Mapping> create(ModelPoint minimum, ModelPoint maximum, double cell,
                                  ValidationPolicy policy,
                                  CancelCheck cancel = nullptr, void *context = nullptr) noexcept;

    ModelPoint minimum() const noexcept { return minimum_; }
    ModelPoint maximum() const noexcept { return maximum_; }
    ModelPoint origin() const noexcept { return minimum_; }
    double cell() const noexcept { return cell_; }
    Index width() const noexcept { return width_; }
    Index height() const noexcept { return height_; }
    Convention convention() const noexcept { return Convention::Binary64SeparateRoundNearestEven; }

    Result<CellIndex> locate(ModelPoint point) const noexcept;
    Result<CellBounds> cellBounds(CellIndex index) const noexcept;

private:
    Mapping(ModelPoint minimum, ModelPoint maximum, double cell, Index width, Index height) noexcept;
    ModelPoint minimum_;
    ModelPoint maximum_;
    double cell_;
    Index width_;
    Index height_;
};

} // namespace cadcontour::raster
