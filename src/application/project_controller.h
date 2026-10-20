#pragma once

#include "core/project_state.h"
#include "operation_runner.h"
#include "import_operation.h"

#include <QObject>
#include <QUndoStack>
#include <QElapsedTimer>
#include <QPointer>

#include <optional>

namespace cadcontour {

struct SaveRequest {
    QString manifestPath; // first Save and Save As require an absolute path
    persistence::ManifestLimits limits{4ULL << 20, 4096, 32};
};

struct OpenRequest {
    QString manifestPath;
    persistence::ManifestLimits limits{4ULL << 20, 4096, 32};
    cache::ResourceLimits readerLimits{1ULL << 20, 0, 2};
    bool replacementConfirmed = false;
    std::optional<ImportContext> expectedContext;
};

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
    // Owner-thread consumer registration; detach/destruction cancels an active import.
    CommandResult setNonZeroZConsumer(QObject *consumer);
    std::optional<NonZeroZRequest> pendingNonZeroZ() const;
    // Stale, duplicate or invalid responses return Rejected/InvalidValue.
    CommandResult answerNonZeroZ(const NonZeroZRequest &request, io::ZDecision decision);

    CommandResult newProject();
    CommandResult importAscXyz(ImportRequest request);
    CommandResult saveProject(SaveRequest request = {});
    CommandResult saveProjectAs(SaveRequest request);
    CommandResult openProject(OpenRequest request);
    CommandResult recheckPointCache(cache::ResourceLimits limits = {1ULL << 20, 0, 2});
    const persistence::CacheAvailability *pointCacheAvailability() const noexcept;
    bool isDirty() const noexcept;
    const persistence::SavedProject *savedProject() const noexcept;
    const persistence::SavedProject *unconfirmedSave() const noexcept;
    // UI captures this before file selection; startImport rechecks it before launching.
    std::optional<ImportContext> importContext() const noexcept;
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
    void nonZeroZConfirmationChanged();

private:
    friend class ProjectControllerTestAccess;
    friend class OperationTestAccess;
    friend class SaveTestAccess;
    friend class OpenTestAccess;
    class CommandScope;
    // Reverse destruction order: undo commands disappear before their referenced state.
    // DatasetLocation retains accepted files across session/controller destruction.
    struct ProjectSession {
        explicit ProjectSession(ProjectState value) : state(std::move(value)) {}
        ProjectState state;
        QUndoStack history;
        std::shared_ptr<const persistence::SavedProject> saved;
        std::shared_ptr<const persistence::SavedProject> unconfirmed;
        std::shared_ptr<const persistence::CacheAvailability> availability =
            std::make_shared<const persistence::CacheAvailability>();
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
    CommandResult startSave(SaveRequest request, bool saveAs);
    bool cacheAvailable() const noexcept;
    void processSaveReceipt(OperationId id, std::shared_ptr<const persistence::SavedProject> prepared,
                            persistence::SaveCommitResult receipt);
    std::unique_ptr<ProjectSession> prepareImport(const ImportReadyProduct &product);
    void invalidateOperation();
    bool applicable(OperationId id) const noexcept;
    void processReady(OperationId id);
    void finishOperation(OperationId id, WorkerStatus status, OperationFailure failure, bool warning);
    void finishCommand();
    void notifyNonZeroZConfirmation();

    std::unique_ptr<ProjectSession> session_;
    QString storageRoot_;
    QString importSourcePath_;
    OperationSnapshot snapshot_;
    std::uint64_t contextEpoch_ = 0;
    // Internal fault seam; tests can cancel/invalidate or fail during preparation.
    std::function<void()> prepareImportHook_;
    std::function<void()> prepareOpenHook_;
    std::function<persistence::SaveResult<std::unique_ptr<persistence::SaveTransaction>>(
        persistence::SaveInput, persistence::SaveCallbacks)> savePrepare_;
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
    bool confirmationNotificationPending_ = false;
    QPointer<QObject> zConsumer_;
    QMetaObject::Connection zConsumerDestroyed_;
};

} // namespace cadcontour
