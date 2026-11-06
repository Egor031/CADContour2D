#pragma once

#include "command_result.h"
#include "imported_dataset.h"
#include "project_id.h"
#include "density_version.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

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
    using DensityCatalogue = std::vector<std::shared_ptr<const DensityVersion>>;
    class PreparedDensityAppend {
    public:
        PreparedDensityAppend(PreparedDensityAppend &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), revision_(other.revision_),
              catalogue_(std::move(other.catalogue_)), active_(other.active_),
              committed_(std::exchange(other.committed_, true)) {}
        PreparedDensityAppend &operator=(PreparedDensityAppend &&other) noexcept
        {
            if (this != &other) {
                owner_ = std::exchange(other.owner_, nullptr);
                revision_ = other.revision_;
                catalogue_ = std::move(other.catalogue_);
                active_ = other.active_;
                committed_ = std::exchange(other.committed_, true);
            }
            return *this;
        }
        PreparedDensityAppend(const PreparedDensityAppend &) = delete;
        PreparedDensityAppend &operator=(const PreparedDensityAppend &) = delete;
    private:
        friend class ProjectState;
        PreparedDensityAppend(const ProjectState *owner, std::uint64_t revision,
                              DensityCatalogue catalogue, VersionId active) noexcept
            : owner_(owner), revision_(revision), catalogue_(std::move(catalogue)), active_(active) {}
        const ProjectState *owner_;
        std::uint64_t revision_;
        DensityCatalogue catalogue_;
        VersionId active_;
        bool committed_ = false;
    };

    explicit ProjectState(std::uint64_t runtimeIdentity);
    ProjectState(std::uint64_t runtimeIdentity, ProjectId projectId, ImportedDataset dataset);
    ProjectState(std::uint64_t runtimeIdentity, ProjectId projectId, ImportedDataset dataset, double savedCell);
    const ImportedDataset *dataset() const noexcept { return dataset_ ? &*dataset_ : nullptr; }
    ProjectId projectId() const noexcept { return projectId_; }

    double cell() const noexcept { return cell_; }
    std::uint64_t runtimeIdentity() const noexcept { return runtimeIdentity_; }
    std::uint64_t inputRevision() const noexcept { return inputRevision_; }
    OperationStamp operationStamp() const noexcept { return {runtimeIdentity_, inputRevision_}; }
    bool accepts(OperationStamp stamp) const noexcept { return operationStamp() == stamp; }

    static bool isValidCell(double value) noexcept;
    CommandResult checkCellChange(double value) const noexcept;
    CommandResult changeCell(double value) noexcept;
    const DensityCatalogue &densityVersions() const noexcept { return densityVersions_; }
    std::optional<VersionId> activeDensityVersion() const noexcept { return activeDensityVersion_; }
    const DensityVersion *densityVersion(VersionId id) const noexcept;
    bool hasUnpersistableV1State() const noexcept { return !densityVersions_.empty(); }
    CommandResult checkDensityAppend(const DensityVersion &version) const noexcept;
    // Preparation may allocate; it never publishes a catalogue or changes content.
    PreparedDensityAppend prepareDensityAppend(std::shared_ptr<const DensityVersion> version) const;
    bool densityAppendApplicable(const PreparedDensityAppend &prepared) const noexcept;
    // The old catalogue remains in prepared until it is destroyed outside the gate.
    void commitDensityAppend(PreparedDensityAppend &prepared) noexcept;
    // Null is the valid target of Undo of the first Build, not a version identity.
    CommandResult checkDensitySelection(std::optional<VersionId> id) const noexcept;
    CommandResult selectDensityVersion(std::optional<VersionId> id) noexcept;
    // Storage relocation preserves domain content, runtime stamps and Undo references.
    void bindDataset(std::shared_ptr<const DatasetLocation> location) noexcept;

private:
    friend class ProjectStateTestAccess;
    std::uint64_t runtimeIdentity_;
    ProjectId projectId_;
    std::uint64_t inputRevision_ = 0;
    double cell_ = 1.0;
    std::optional<ImportedDataset> dataset_;
    DensityCatalogue densityVersions_;
    std::optional<VersionId> activeDensityVersion_;
};

} // namespace cadcontour
