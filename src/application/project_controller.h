#pragma once

#include "core/project_state.h"
#include "operation_runner.h"
#include "import_operation.h"

#include <QObject>
#include <QUndoStack>
#include <QElapsedTimer>

#include <optional>

namespace cadcontour {

class ProjectController : public QObject {
    Q_OBJECT

public:
    explicit ProjectController(QObject *parent = nullptr);
    explicit ProjectController(QString storageRoot, QObject *parent = nullptr);
    ~ProjectController() override;

    const ProjectState *project() const noexcept;
    bool canUndo() const noexcept { return session_ && session_->history.canUndo(); }
    bool canRedo() const noexcept { return session_ && session_->history.canRedo(); }
    const OperationState &operation() const noexcept { return operation_; }
    ShutdownState shutdownState() const noexcept { return shutdown_; }

    CommandResult newProject();
    CommandResult importAscXyz(ImportRequest request);
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
    // Reverse destruction order: undo commands disappear before their referenced state.
    // Storage retains accepted files across session/controller destruction (no GC before task 05).
    struct PersistentStorage {
        QString directory;
        QString relativeCachePath;
    };
    struct ProjectSession {
        explicit ProjectSession(ProjectState value, PersistentStorage owner = {})
            : state(std::move(value)), storage(std::move(owner)) {}
        ProjectState state;
        PersistentStorage storage;
        QUndoStack history;
    };
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
    CommandResult startWork(OperationWork work, OperationKind kind, bool replacementConfirmed = false);
    CommandResult startImport(ImportRequest request, io::ZHandler handler);
    std::unique_ptr<ProjectSession> prepareImport(const ImportReadyProduct &product);
    void invalidateOperation();
    bool applicable(OperationId id) const noexcept;
    void processReady(OperationId id);
    void finishOperation(OperationId id, WorkerStatus status, OperationFailure failure, bool warning);
    void finishCommand();

    std::unique_ptr<ProjectSession> session_;
    QString storageRoot_;
    QString importSourcePath_;
    OperationSnapshot snapshot_;
    std::uint64_t contextEpoch_ = 0;
    // Internal fault seam; tests can cancel/invalidate or fail during preparation.
    std::function<void()> prepareImportHook_;
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
