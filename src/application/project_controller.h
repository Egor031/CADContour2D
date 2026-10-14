#pragma once

#include "core/project_state.h"
#include "operation_runner.h"

#include <QObject>
#include <QUndoStack>
#include <QElapsedTimer>

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
    const OperationState &operation() const noexcept { return operation_; }
    ShutdownState shutdownState() const noexcept { return shutdown_; }

    CommandResult newProject();
    CommandResult closeProject();
    CommandResult setCell(double value);
    CommandResult undo();
    CommandResult redo();
    CommandResult cancelOperation();
    CommandResult requestShutdown();

signals:
    // Emitted once with both domain state and undo history settled; reentry is rejected.
    void stateChanged();
    void operationChanged();
    void shutdownChanged();

private:
    friend class ProjectControllerTestAccess;
    friend class OperationTestAccess;
    class CommandScope;
    struct VerificationState {
        std::uint64_t generation = 0;
        double sourceCell = 0;
        std::uint64_t value = 0;
        std::unique_ptr<VerificationProduct> acceptedProduct;
    };
    CommandResult checkEntry() const noexcept;
    CommandResult moveHistory(bool undo);
    CommandResult startOperation(OperationWork work,
                                std::function<void(VerificationState &)> finalize = {});
    void invalidateOperation();
    bool applicable(OperationId id) const noexcept;
    void processReady(OperationId id);
    void finishOperation(OperationId id, WorkerStatus status, OperationFailure failure, bool warning);
    void finishCommand();

    std::optional<ProjectState> project_;
    QUndoStack history_;
    std::uint64_t lastIdentity_ = 0;
    bool executing_ = false;
    OperationRunner runner_;
    OperationState operation_;
    OperationId lastOperationId_ = 0;
    ShutdownState shutdown_ = ShutdownState::Open;
    QElapsedTimer operationClock_;
    std::function<void(VerificationState &)> finalize_;
    std::unique_ptr<VerificationState> verification_;
    std::unique_ptr<VerificationState> previousVerification_;
    bool completionPending_ = false;
    bool operationNotificationPending_ = false;
};

} // namespace cadcontour
