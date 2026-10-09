#include "project_controller.h"

#include <QScopedValueRollback>
#include <QUndoCommand>

#include <cassert>
#include <limits>
#include <memory>

namespace cadcontour {
namespace {

class SetCellCommand final : public QUndoCommand {
public:
    SetCellCommand(ProjectState &state, double value)
        : state_(state), before_(state.cell()), after_(value)
    {}

    void undo() override { apply(before_); }
    void redo() override { apply(after_); }

private:
    void apply(double value) noexcept
    {
        // Controller preflights every push/undo/redo before moving the stack.
        const auto result = state_.changeCell(value);
        assert(result.outcome == CommandOutcome::Applied);
        (void)result;
    }

    ProjectState &state_;
    double before_;
    double after_;
};

} // namespace

ProjectController::ProjectController(QObject *parent) : QObject(parent) {}

ProjectController::~ProjectController()
{
    history_.clear();
}

const ProjectState *ProjectController::project() const noexcept
{
    return project_ ? &*project_ : nullptr;
}

CommandResult ProjectController::checkEntry() const noexcept
{
    if (executing_)
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::ReentrantCommand);
    return CommandResult::applied();
}

CommandResult ProjectController::newProject()
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (lastIdentity_ == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::IdentityExhausted);
    QScopedValueRollback guard(executing_, true);
    history_.clear();
    project_.emplace(++lastIdentity_);
    emit stateChanged();
    return CommandResult::applied();
}

CommandResult ProjectController::closeProject()
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!project_)
        return CommandResult::noChange();
    QScopedValueRollback guard(executing_, true);
    history_.clear();
    project_.reset();
    emit stateChanged();
    return CommandResult::applied();
}

CommandResult ProjectController::setCell(double value)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!project_)
        return CommandResult::rejected(CommandError::NoProject);
    const auto result = project_->checkCellChange(value);
    if (result.outcome != CommandOutcome::Applied)
        return result;
    if (history_.index() == std::numeric_limits<int>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::HistoryExhausted);
    auto command = std::make_unique<SetCellCommand>(*project_, value);
    QScopedValueRollback guard(executing_, true);
    history_.push(command.release()); // QUndoStack owns the command and calls redo().
    emit stateChanged();
    return result;
}

CommandResult ProjectController::undo() { return moveHistory(true); }
CommandResult ProjectController::redo() { return moveHistory(false); }

CommandResult ProjectController::moveHistory(bool undo)
{
    const auto entry = checkEntry();
    if (entry.outcome == CommandOutcome::Rejected)
        return entry;
    if (!project_)
        return CommandResult::rejected(CommandError::NoProject);
    if (!(undo ? history_.canUndo() : history_.canRedo()))
        return CommandResult::noChange();
    if (project_->inputRevision() == std::numeric_limits<std::uint64_t>::max())
        return CommandResult::rejected(CommandError::UnsafeState, ErrorDetail::RevisionExhausted);
    QScopedValueRollback guard(executing_, true);
    if (undo)
        history_.undo();
    else
        history_.redo();
    emit stateChanged();
    return CommandResult::applied();
}

} // namespace cadcontour
