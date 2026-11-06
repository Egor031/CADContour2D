#pragma once

#include "core/project_state.h"
#include "operation_runner.h"
#include "import_operation.h"
#include "density_operation.h"
#include "project_history.h"

#include <QObject>
#include <QElapsedTimer>
#include <QPointer>

#include <optional>

namespace cadcontour {

enum class DensityTransitionKind { Select, Undo, Redo, Build };
enum class PreciseResetDecision { NeedsConfirmation, Cancel, Confirm, Stale };
struct PreciseResetRequest {
    DensityTransitionKind action;
    ProjectId projectId;
    std::uint64_t runtimeIdentity, sessionEpoch;
    std::optional<VersionId> before, target;
    std::uint64_t preciseGeneration, historyGeneration, choiceGeneration, roughRevision;
    bool operator==(const PreciseResetRequest &) const = default;
};

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
struct RecoveryRequest {
    persistence::RecoveryCandidate candidate;
    persistence::RecoveryLimits limits;
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
    // Explicit override/detach seam; otherwise the session supplies its production receiver.
    CommandResult setDensityReceiver(std::shared_ptr<DensityPublicationReceiver> receiver);
    CommandResult buildDensity(DensityBuildRequest request = {});
    CommandResult selectDensityVersion(VersionId id);
    // No Precise data exist in 06.4. This policy boundary is shared by direct
    // selection, history traversal and Build, ready for the future stage owner.
    CommandResult setPreciseResetHandler(std::function<PreciseResetDecision(const PreciseResetRequest &)> handler);
    CommandResult saveProject(SaveRequest request = {});
    CommandResult saveProjectAs(SaveRequest request);
    CommandResult openProject(OpenRequest request);
    CommandResult recheckPointCache(cache::ResourceLimits limits = {1ULL << 20, 0, 2});
    const persistence::CacheAvailability *pointCacheAvailability() const noexcept;
    bool isDirty() const noexcept;
    const persistence::SavedProject *savedProject() const noexcept;
    const persistence::SavedProject *unconfirmedSave() const noexcept;
    CommandResult autosave(); // timer tick: Busy is skipped, never queued
    CommandResult discoverRecovery(persistence::RecoveryLimits limits = {});
    const std::vector<persistence::RecoveryCandidate> &recoveryCandidates() const noexcept;
    CommandResult restoreRecovery(RecoveryRequest request);
    CommandResult rejectRecovery(persistence::RecoveryCandidate candidate, persistence::RecoveryLimits limits = {});
    const persistence::RecoveryRecord *recoveryRecord() const noexcept;
    bool recoveryRequiresSaveAs() const noexcept;
    // UI captures this before file selection; startImport rechecks it before launching.
    std::optional<ImportContext> importContext() const noexcept;
    // Intent applicability during notifications/Autosave, independent of admission.
    std::optional<ImportContext> importSelectionContext() const noexcept;
    // Read-only lifecycle context even while Busy; this never grants admission.
    std::optional<ImportContext> fileCommandContext() const noexcept;
    CommandResult closeProject(bool discardConfirmed = false);
    CommandResult setCell(double value);
    CommandResult undo();
    CommandResult redo();
    CommandResult cancelOperation();
    CommandResult requestShutdown(bool discardConfirmed = false);

signals:
    // Emitted once with both domain state and undo history settled; reentry is rejected.
    void stateChanged();
    void operationChanged();
    void shutdownChanged();
    void nonZeroZConfirmationChanged();
    void recoveryChanged();
    // Retry hint after an outer CommandScope releases its guard, not an admission grant.
    void commandTransitionFinished();

private:
    friend class ProjectControllerTestAccess;
    friend class OperationTestAccess;
    friend class SaveTestAccess;
    friend class OpenTestAccess;
    friend class RecoveryTestAccess;
    friend class DensityOperationTestAccess;
    friend class DensityVersionTestAccess;
    class CommandScope;
    class VersionReceiver;
    class VersionCandidate;
    // Reverse destruction order: undo commands disappear before their referenced state.
    // DatasetLocation retains accepted files across session/controller destruction.
    struct ProjectSession {
        explicit ProjectSession(ProjectState value) : state(std::move(value)) {}
        ProjectState state;
        ProjectHistory history;
        std::shared_ptr<DensityPublicationReceiver> densityReceiver;
        bool preciseResetRequired = false;
        std::uint64_t preciseGeneration = 0;
        std::uint64_t densityChoiceGeneration = 0;
        std::shared_ptr<const persistence::SavedProject> saved;
        std::shared_ptr<const persistence::SavedProject> unconfirmed;
        std::shared_ptr<const persistence::CacheAvailability> availability =
            std::make_shared<const persistence::CacheAvailability>();
        QUuid recoveryId = QUuid::createUuid();
        std::shared_ptr<const persistence::RecoveryRecord> recovery;
        std::shared_ptr<const persistence::RecoveryLease> recoveryLease;
        bool recoveryConflict = false;
    };
    struct VerificationState {
        std::uint64_t generation = 0;
        double sourceCell = 0;
        std::uint64_t value = 0;
        std::unique_ptr<VerificationProduct> acceptedProduct;
    };
    CommandResult checkEntry() const noexcept;
    CommandResult moveHistory(bool undo);
    PreciseResetRequest densityTransition(DensityTransitionKind action, std::optional<VersionId> target) const noexcept;
    CommandResult preparePreciseReset(const PreciseResetRequest &request, bool &reset);
    bool transitionApplicable(const PreciseResetRequest &request, bool checkHistory = true) const noexcept;
    void commitPreciseReset(bool reset) noexcept;
    CommandResult startOperation(OperationWork work,
                                std::function<void(VerificationState &)> finalize = {});
    CommandResult startWork(OperationWork work, OperationKind kind, bool replacementConfirmed = false,
                            std::shared_ptr<const DensityBuildSnapshot> density = {},
                            std::shared_ptr<DensityPublicationReceiver> receiver = {});
    CommandResult startImport(ImportRequest request, io::ZHandler handler);
    CommandResult startSave(SaveRequest request, bool saveAs);
    bool cacheAvailable() const noexcept;
    void processSaveReceipt(OperationId id, std::shared_ptr<const persistence::SavedProject> prepared,
                            persistence::SaveCommitResult receipt);
    std::shared_ptr<ProjectSession> prepareImport(const ImportReadyProduct &product);
    void invalidateOperation();
    bool applicable(OperationId id) const noexcept;
    void processReady(OperationId id);
    void processDensityReady(OperationId id, ReadyProductLease &lease);
    bool densityApplicable(OperationId id) const noexcept;
    void finishOperation(OperationId id, WorkerStatus status, OperationFailure failure, bool warning);
    void finishCommand();
    void notifyNonZeroZConfirmation();
    void processRecoveryReceipt(OperationId id, std::shared_ptr<const persistence::RecoveryRecord> record,
        std::shared_ptr<const persistence::RecoveryLease> lease, persistence::RecoveryReceipt receipt);
    void retireRecovery(bool discardConfirmed);
    void finishRecoverySessions();
    bool preemptAutosave();

    std::shared_ptr<ProjectSession> session_;
    QString storageRoot_;
    QString recoveryRoot_;
    QTimer autosaveTimer_;
    QTimer autosaveSoon_;
    struct RetiredRecovery {
        QUuid id;
        std::shared_ptr<const persistence::RecoveryLease> lease;
        bool discard = false;
    };
    std::vector<RetiredRecovery> retiredRecoveries_;
    std::shared_ptr<const std::vector<persistence::RecoveryCandidate>> recoveryCandidates_;
    bool shutdownDiscard_ = false;
    bool recoveryClosing_ = false;
    std::function<persistence::Result<std::unique_ptr<persistence::RecoveryTransaction>>(
        QString, persistence::RecoveryRecord, std::shared_ptr<const persistence::RecoveryLease>,
        persistence::RecoveryLimits, std::function<bool()>)> recoveryPrepare_;
    QString importSourcePath_;
    std::shared_ptr<DensityPublicationReceiver> densityReceiver_;
    bool densityReceiverOverridden_ = false;
    std::function<VersionId()> versionIdFactory_;
    std::function<void()> prepareDensityHook_;
    std::function<void()> prepareHistoryHook_;
    std::function<PreciseResetDecision(const PreciseResetRequest &)> preciseResetHandler_;
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
    OperationId lastImportContextOperation_ = 0;
    ShutdownState shutdown_ = ShutdownState::Open;
    QElapsedTimer operationClock_;
    std::function<void(VerificationState &)> finalize_;
    std::unique_ptr<VerificationState> verification_;
    std::unique_ptr<VerificationState> previousVerification_;
    bool completionPending_ = false;
    bool operationNotificationPending_ = false;
    bool confirmationNotificationPending_ = false;
    bool transitionNotificationPosted_ = false;
    QPointer<QObject> zConsumer_;
    QMetaObject::Connection zConsumerDestroyed_;
};

} // namespace cadcontour
