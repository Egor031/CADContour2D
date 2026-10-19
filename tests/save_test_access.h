#pragma once

#include "application/project_controller.h"

namespace cadcontour::persistence {
class SaveTransactionTestAccess {
public:
    using PhaseHook = std::function<void(SavePhase)>;
    using WriteHook = std::function<qint64(QIODevice &, const char *, qint64)>;
    using CommitHook = std::function<bool(QSaveFile &)>;
    static SaveResult<std::unique_ptr<SaveTransaction>> prepare(SaveInput input, SaveCallbacks callbacks,
        PhaseHook phase = {}, WriteHook write = {}, CommitHook commit = {})
    {
        auto hooks = std::make_shared<SaveTransaction::TestHooks>();
        hooks->phase = std::move(phase); hooks->write = std::move(write); hooks->commit = std::move(commit);
        return SaveTransaction::prepareImpl(std::move(input), std::move(callbacks), std::move(hooks));
    }
};
}
namespace cadcontour {
class SaveTestAccess {
public:
    static void configure(ProjectController &controller, persistence::SaveTransactionTestAccess::PhaseHook phase = {},
        persistence::SaveTransactionTestAccess::WriteHook write = {},
        persistence::SaveTransactionTestAccess::CommitHook commit = {})
    {
        controller.savePrepare_ = [phase = std::move(phase), write = std::move(write), commit = std::move(commit)]
            (auto input, auto callbacks) {
                return persistence::SaveTransactionTestAccess::prepare(std::move(input), std::move(callbacks), phase, write, commit);
            };
    }
    static std::shared_ptr<const persistence::SavedProject> saved(ProjectController &controller)
    { return controller.session_->saved; }
    static void installFixture(ProjectController &controller, ProjectState state,
                               std::shared_ptr<const persistence::SavedProject> saved = {})
    {
        auto session = std::make_unique<ProjectController::ProjectSession>(std::move(state));
        session->saved = std::move(saved);
        controller.lastIdentity_ = session->state.runtimeIdentity();
        controller.session_ = std::move(session);
        ++controller.contextEpoch_;
    }
};
}
