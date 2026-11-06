#include "project_state.h"

#include <cmath>
#include <bit>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace cadcontour {

ProjectState::ProjectState(std::uint64_t runtimeIdentity) : runtimeIdentity_(runtimeIdentity)
{
    if (runtimeIdentity == 0)
        throw std::invalid_argument("Project runtime identity must be nonzero");
}

ProjectState::ProjectState(std::uint64_t runtimeIdentity, ProjectId projectId, ImportedDataset dataset)
    : ProjectState(runtimeIdentity)
{
    if (projectId.isNull())
        throw std::invalid_argument("Project persistent identity must be nonzero");
    projectId_ = projectId;
    dataset_.emplace(std::move(dataset));
}

bool ProjectState::isValidCell(double value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

ProjectState::ProjectState(std::uint64_t runtimeIdentity, ProjectId projectId,
                           ImportedDataset dataset, double savedCell)
    : ProjectState(runtimeIdentity, projectId, std::move(dataset))
{
    if (!isValidCell(savedCell))
        throw std::invalid_argument("Saved cell must be finite and positive");
    cell_ = savedCell;
}

void ProjectState::bindDataset(std::shared_ptr<const DatasetLocation> location) noexcept
{
    if (dataset_) dataset_->location = std::move(location);
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

const DensityVersion *ProjectState::densityVersion(VersionId id) const noexcept
{
    if (id.isNull()) return nullptr;
    for (const auto &version : densityVersions_)
        if (version->descriptor().id == id) return version.get();
    return nullptr;
}

CommandResult ProjectState::checkDensityAppend(const DensityVersion &version) const noexcept
{
    const auto &descriptor = version.descriptor();
    const auto invalid = [] {
        return CommandResult::rejected(CommandError::InvalidValue, ErrorDetail::InvalidDensityVersion);
    };
    const bool nullSource = std::all_of(descriptor.source.bytes.begin(), descriptor.source.bytes.end(),
                                       [](auto b) { return b == std::byte{}; });
    if (descriptor.id.isNull() || descriptor.dataset.id.isNull() || nullSource
        || !descriptor.dataset.location || descriptor.dataset.location->path.empty()
        || !descriptor.dataset.location->path.is_absolute() || !dataset_
        || descriptor.source != dataset_->cacheId
        || descriptor.sourceMeasurements == 0 || descriptor.sourceMeasurements != dataset_->pointCount)
        return invalid();
    const auto same = [](double a, double b) {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    };
    const auto minimum = descriptor.mapping.minimum(), maximum = descriptor.mapping.maximum();
    if (!same(minimum.x, dataset_->minimum.x) || !same(minimum.y, dataset_->minimum.y)
        || !same(maximum.x, dataset_->maximum.x) || !same(maximum.y, dataset_->maximum.y)
        || descriptor.build.method != DensityBuildMethod::C1
        || descriptor.build.contract != DensityCountContract::CompletePointCache
        || descriptor.build.densityFormatVersion != 1 || descriptor.build.tileSide == 0
        || descriptor.build.groupSide == 0 || descriptor.build.eventCapacity == 0)
        return invalid();
    for (const auto &existing : densityVersions_) {
        if (existing->descriptor().id == descriptor.id
            || existing->descriptor().dataset.id == descriptor.dataset.id)
            return CommandResult::rejected(CommandError::InvalidValue, ErrorDetail::DuplicateDensityVersion);
    }
    if (inputRevision_ == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    if (densityVersions_.size() == densityVersions_.max_size())
        return CommandResult::rejected(CommandError::ResourceUnavailable, ErrorDetail::OutOfMemory);
    return CommandResult::applied();
}

ProjectState::PreparedDensityAppend ProjectState::prepareDensityAppend(
    std::shared_ptr<const DensityVersion> version) const
{
    if (!version || checkDensityAppend(*version).outcome != CommandOutcome::Applied)
        throw std::invalid_argument("Density version cannot be appended");
    DensityCatalogue catalogue;
    catalogue.reserve(densityVersions_.size() + 1);
    catalogue.insert(catalogue.end(), densityVersions_.begin(), densityVersions_.end());
    const auto id = version->descriptor().id;
    catalogue.push_back(std::move(version));
    return PreparedDensityAppend(this, inputRevision_, std::move(catalogue), id);
}

bool ProjectState::densityAppendApplicable(const PreparedDensityAppend &prepared) const noexcept
{
    return prepared.owner_ == this && !prepared.committed_
        && prepared.revision_ == inputRevision_
        && inputRevision_ != std::numeric_limits<std::uint64_t>::max();
}

void ProjectState::commitDensityAppend(PreparedDensityAppend &prepared) noexcept
{
    assert(densityAppendApplicable(prepared));
    densityVersions_.swap(prepared.catalogue_);
    activeDensityVersion_ = prepared.active_;
    ++inputRevision_;
    prepared.committed_ = true;
}

CommandResult ProjectState::checkDensitySelection(std::optional<VersionId> id) const noexcept
{
    if (id && !densityVersion(*id))
        return CommandResult::rejected(CommandError::InvalidValue, ErrorDetail::InvalidDensityVersion);
    if (id == activeDensityVersion_) return CommandResult::noChange();
    if (inputRevision_ == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    return CommandResult::applied();
}

CommandResult ProjectState::selectDensityVersion(std::optional<VersionId> id) noexcept
{
    const auto result = checkDensitySelection(id);
    if (result.outcome == CommandOutcome::Applied) {
        activeDensityVersion_ = id;
        ++inputRevision_;
    }
    return result;
}

} // namespace cadcontour
