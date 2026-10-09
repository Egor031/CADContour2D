#pragma once

#include "application/project_controller.h"

namespace cadcontour {

class ProjectStateTestAccess {
public:
    static void setRevision(ProjectState &state, std::uint64_t value) { state.inputRevision_ = value; }
};

class ProjectControllerTestAccess {
public:
    static void setRevision(ProjectController &controller, std::uint64_t value)
    {
        ProjectStateTestAccess::setRevision(*controller.project_, value);
    }
    static void setLastIdentity(ProjectController &controller, std::uint64_t value)
    {
        controller.lastIdentity_ = value;
    }
    static int count(const ProjectController &controller) { return controller.history_.count(); }
    static int index(const ProjectController &controller) { return controller.history_.index(); }
};

} // namespace cadcontour
