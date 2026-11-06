#pragma once

#include "core/version_id.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace cadcontour {

enum class ProjectCommandKind { SetCell, DensitySelection };
struct ProjectHistoryEntry {
    ProjectCommandKind kind = ProjectCommandKind::SetCell;
    double beforeCell = 0, afterCell = 0;
    std::optional<VersionId> beforeVersion, afterVersion;
};

// Entries retain identities only. Preparation may allocate; publication swaps
// owners, leaving the old branch with the prepared object until after the gate.
class ProjectHistory {
public:
    bool canUndo() const noexcept { return index_ != 0; }
    bool canRedo() const noexcept { return index_ != count(); }
    int index() const noexcept { return index_; }
    int count() const noexcept { return static_cast<int>(entries_.size()); }
    std::uint64_t generation() const noexcept { return generation_; }
    bool canAdvance() const noexcept { return generation_ != std::numeric_limits<std::uint64_t>::max(); }
    bool canAppend() const noexcept { return canAdvance() && index_ != std::numeric_limits<int>::max(); }
    const ProjectHistoryEntry &command(int index) const noexcept { return entries_[index]; }
    ProjectHistory prepare(ProjectHistoryEntry entry) const
    {
        if (!canAppend()) throw std::overflow_error("project history exhausted");
        ProjectHistory next;
        next.entries_.reserve(static_cast<std::size_t>(index_) + 1);
        next.entries_.assign(entries_.begin(), entries_.begin() + index_);
        next.entries_.push_back(entry);
        next.index_ = index_ + 1;
        next.generation_ = generation_ + 1;
        return next;
    }
    void publish(ProjectHistory &prepared) noexcept
    {
        entries_.swap(prepared.entries_);
        std::swap(index_, prepared.index_);
        std::swap(generation_, prepared.generation_);
    }
    void move(bool undo) noexcept { index_ += undo ? -1 : 1; ++generation_; }
private:
    friend class DensityVersionTestAccess;
    std::vector<ProjectHistoryEntry> entries_;
    int index_ = 0;
    std::uint64_t generation_ = 0;
};

} // namespace cadcontour
