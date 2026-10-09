#pragma once

#include "core/project_state.h"

#include <QObject>
#include <QUndoStack>

#include <optional>

namespace cadcontour {

class ProjectController : public QObject {
    Q_OBJECT

public:
    explicit ProjectController(QObject *parent = nullptr);
    ~ProjectController() override;

    const ProjectState *project() const noexcept;
    bool canUndo() const noexcept { return history_.canUndo(); }
    bool canRedo() const noexcept { return history_.canRedo(); }

    CommandResult newProject();
    CommandResult closeProject();
    CommandResult setCell(double value);
    CommandResult undo();
    CommandResult redo();

signals:
    // Emitted once with both domain state and undo history settled; reentry is rejected.
    void stateChanged();

private:
    friend class ProjectControllerTestAccess;
    CommandResult checkEntry() const noexcept;
    CommandResult moveHistory(bool undo);

    std::optional<ProjectState> project_;
    QUndoStack history_;
    std::uint64_t lastIdentity_ = 0;
    bool executing_ = false;
};

} // namespace cadcontour
