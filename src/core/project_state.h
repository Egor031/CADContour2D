#pragma once

#include "command_result.h"

#include <cstdint>

namespace cadcontour {

enum class ResultStatus { Missing, Current, Outdated };

// Compatibility is supplied by the result's provenance contract, not a worker revision.
constexpr ResultStatus resultStatus(bool present, bool compatible) noexcept
{
    return !present ? ResultStatus::Missing
                    : (compatible ? ResultStatus::Current : ResultStatus::Outdated);
}

struct OperationStamp {
    std::uint64_t projectIdentity;
    std::uint64_t inputRevision;
    bool operator==(const OperationStamp &) const = default;
};

class ProjectState {
public:
    explicit ProjectState(std::uint64_t runtimeIdentity);

    double cell() const noexcept { return cell_; }
    std::uint64_t runtimeIdentity() const noexcept { return runtimeIdentity_; }
    std::uint64_t inputRevision() const noexcept { return inputRevision_; }
    OperationStamp operationStamp() const noexcept { return {runtimeIdentity_, inputRevision_}; }
    bool accepts(OperationStamp stamp) const noexcept { return operationStamp() == stamp; }

    static bool isValidCell(double value) noexcept;
    CommandResult checkCellChange(double value) const noexcept;
    CommandResult changeCell(double value) noexcept;

private:
    friend class ProjectStateTestAccess;
    std::uint64_t runtimeIdentity_;
    std::uint64_t inputRevision_ = 0;
    double cell_ = 1.0;
};

} // namespace cadcontour
