#include "project_state.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace cadcontour {

ProjectState::ProjectState(std::uint64_t runtimeIdentity) : runtimeIdentity_(runtimeIdentity)
{
    if (runtimeIdentity == 0)
        throw std::invalid_argument("Project runtime identity must be nonzero");
}

bool ProjectState::isValidCell(double value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

CommandResult ProjectState::checkCellChange(double value) const noexcept
{
    if (!isValidCell(value))
        return CommandResult::rejected(CommandError::InvalidValue,
                                       ErrorDetail::CellMustBeFiniteAndPositive, value);
    // Exact equality is intentional for a parameter assignment, not a geometric decision.
    if (value == cell_)
        return CommandResult::noChange();
    if (inputRevision_ == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    return CommandResult::applied();
}

CommandResult ProjectState::changeCell(double value) noexcept
{
    const auto result = checkCellChange(value);
    if (result.outcome == CommandOutcome::Applied) {
        cell_ = value;
        ++inputRevision_;
    }
    return result;
}

} // namespace cadcontour
