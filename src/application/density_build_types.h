#pragma once

#include "core/project_id.h"
#include "density/density_builder.h"

#include <memory>

namespace cadcontour {

struct DensityBuildRequest {
    std::uint32_t tileSide = 256, groupSide = 512;
    density::U64 eventCapacity = 16384;
    density::BuildResourcePolicy resources = density::initialBuildResourcePolicy();
};

// The source binding and all inputs are retained independently of ProjectState.
// outputScope is a unique, fully qualified directory which does not yet exist.
struct DensityBuildSnapshot {
    ProjectId projectId;
    ImportedDataset source;
    double cell = 1;
    std::uint32_t tileSide = 256, groupSide = 512;
    density::U64 eventCapacity = 16384;
    density::BuildResourcePolicy resources = density::initialBuildResourcePolicy();
    QString outputScope;
};

struct DensityReadyProduct {
    std::shared_ptr<const DensityBuildSnapshot> snapshot;
    density::Metadata metadata;
    QString completedPath;
    density::BuildStatistics statistics;
};

class DensityPublicationCandidate {
public:
    virtual ~DensityPublicationCandidate() = default;
    virtual const std::shared_ptr<const DensityReadyProduct> &ready() const noexcept = 0;
    // Rechecks application preparation context after callbacks/reentry and before
    // the decision gate. Legacy receivers without mutable context need no check.
    virtual bool applicable() const noexcept { return true; }
    // Prepare reserves all storage and retention. Commit adds the new descriptor
    // and selects it, preserving previous versions and their edits. No I/O,
    // allocation, callbacks or substantial destruction are allowed here.
    virtual void commit() noexcept = 0;
    virtual void notify() noexcept {}
};

class DensityPublicationReceiver {
public:
    virtual ~DensityPublicationReceiver() = default;
    // A null candidate refuses publication (including an unconfirmed Precise
    // reset). Preparation must leave current selection and domain state intact.
    virtual std::unique_ptr<DensityPublicationCandidate> prepare(
        std::shared_ptr<const DensityReadyProduct> ready) = 0;
};

// Bounded immutable metadata checks only; no Mapping factory or file I/O.
bool validDensityReady(const DensityBuildSnapshot &, const DensityReadyProduct &) noexcept;

struct DensityOperationDiagnostics {
    std::optional<density::BuildError> cleanupError;
    bool bridgeFailed = false;
    density::BuildPhase bridgePhase = density::BuildPhase::Preflight;
};

// Worker-only: removes only the two directories it created, never recursively
// traversing an arbitrary/foreign file. Locked or foreign children are diagnosed.
class DensityAttemptDirectory {
public:
    DensityAttemptDirectory(QString path, std::shared_ptr<DensityOperationDiagnostics> diagnostics);
    ~DensityAttemptDirectory();
    DensityAttemptDirectory(const DensityAttemptDirectory &) = delete;
    DensityAttemptDirectory &operator=(const DensityAttemptDirectory &) = delete;
    const QString &path() const noexcept { return path_; }
    const QString &scratchPath() const noexcept { return scratch_; }
    bool create();
    bool created() const noexcept { return created_; }
    bool retained() const noexcept { return retained_; }
    void retain() noexcept { retained_ = true; }
private:
    QString path_, scratch_;
    std::shared_ptr<DensityOperationDiagnostics> diagnostics_;
    bool created_ = false, scratchCreated_ = false, retained_ = false;
};

// Member order and explicit cleanup destroy PendingDataset before its directory.
// Neither object is transported to the owner-thread Ready mailbox.
struct WorkerDensityProduct {
    std::unique_ptr<DensityAttemptDirectory> directory;
    std::unique_ptr<density::BuildOutput> output;
    std::shared_ptr<const DensityReadyProduct> ready;
    std::shared_ptr<DensityOperationDiagnostics> diagnostics;
    bool valid(const DensityBuildSnapshot &) const noexcept;
    void accept() noexcept;
    bool cleanup() noexcept;
};

} // namespace cadcontour
