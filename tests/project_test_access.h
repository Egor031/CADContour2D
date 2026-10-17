#pragma once

#include "application/project_controller.h"

#include <limits>

namespace cadcontour {

class ProjectStateTestAccess {
public:
    static void setRevision(ProjectState &state, std::uint64_t value) { state.inputRevision_ = value; }
};

class ProjectControllerTestAccess {
public:
    // Domain/Undo/verification fixture only. Production creates sessions through import.
    static CommandResult newProject(ProjectController &controller)
    {
        const auto entry = controller.checkEntry();
        if (entry.outcome == CommandOutcome::Rejected)
            return entry;
        if (controller.lastIdentity_ == std::numeric_limits<std::uint64_t>::max())
            return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::IdentityExhausted);
        auto candidate = std::make_unique<ProjectController::ProjectSession>(
            ProjectState(controller.lastIdentity_ + 1));
        controller.executing_ = true;
        controller.invalidateOperation();
        controller.verification_.reset();
        controller.previousVerification_.reset();
        controller.session_.swap(candidate);
        ++controller.lastIdentity_;
        ++controller.contextEpoch_;
        emit controller.stateChanged();
        controller.finishCommand();
        return CommandResult::applied();
    }
    static void setRevision(ProjectController &controller, std::uint64_t value)
    {
        ProjectStateTestAccess::setRevision(controller.session_->state, value);
    }
    static void setLastIdentity(ProjectController &controller, std::uint64_t value)
    {
        controller.lastIdentity_ = value;
    }
    static int count(const ProjectController &controller) { return controller.session_ ? controller.session_->history.count() : 0; }
    static int index(const ProjectController &controller) { return controller.session_ ? controller.session_->history.index() : 0; }
};

} // namespace cadcontour
