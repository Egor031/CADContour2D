#pragma once

#include "application/project_controller.h"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace cadcontour {

class ProjectViewModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(ProjectViewModel)
    QML_UNCREATABLE("ProjectViewModel is supplied by the application")
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY stateChanged)
    Q_PROPERTY(double cell READ cell NOTIFY stateChanged)
    Q_PROPERTY(QString cellText READ cellText NOTIFY stateChanged)
    Q_PROPERTY(QString runtimeIdentity READ runtimeIdentity NOTIFY stateChanged)
    Q_PROPERTY(QString inputRevision READ inputRevision NOTIFY stateChanged)
    Q_PROPERTY(QString densityMapStatusText READ densityMapStatusText NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool actionsEnabled READ actionsEnabled NOTIFY shutdownChanged)
    Q_PROPERTY(bool shutdownReady READ shutdownReady NOTIFY shutdownChanged)
    Q_PROPERTY(bool closing READ closing NOTIFY shutdownChanged)
    Q_PROPERTY(bool operationVisible READ operationVisible NOTIFY operationChanged)
    Q_PROPERTY(bool operationActive READ operationActive NOTIFY operationChanged)
    Q_PROPERTY(bool canCancelOperation READ canCancelOperation NOTIFY operationChanged)
    Q_PROPERTY(bool progressKnown READ progressKnown NOTIFY operationChanged)
    Q_PROPERTY(double progressValue READ progressValue NOTIFY operationChanged)
    Q_PROPERTY(QString operationText READ operationText NOTIFY operationChanged)
    Q_PROPERTY(QString phaseText READ phaseText NOTIFY operationChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY operationChanged)
    Q_PROPERTY(bool canImport READ canImport NOTIFY importUiChanged)
    Q_PROPERTY(QString selectionToken READ selectionToken NOTIFY importUiChanged)
    Q_PROPERTY(bool selectingFile READ selectingFile NOTIFY importUiChanged)
    Q_PROPERTY(bool replacementPending READ replacementPending NOTIFY importUiChanged)
    Q_PROPERTY(QString replacementFileName READ replacementFileName NOTIFY importUiChanged)
    Q_PROPERTY(QString nonZeroZToken READ nonZeroZToken NOTIFY nonZeroZChanged)
    Q_PROPERTY(QString nonZeroZText READ nonZeroZText NOTIFY nonZeroZChanged)
    Q_PROPERTY(bool hasDataset READ hasDataset NOTIFY stateChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY stateChanged)
    Q_PROPERTY(QString pointCountText READ pointCountText NOTIFY stateChanged)
    Q_PROPERTY(QString boundsText READ boundsText NOTIFY stateChanged)
    Q_PROPERTY(bool datasetHasNonZeroZ READ datasetHasNonZeroZ NOTIFY stateChanged)
    Q_PROPERTY(QString diagnosticText READ diagnosticText NOTIFY operationChanged)
    Q_PROPERTY(QVariantMap importDiagnostic READ importDiagnostic NOTIFY operationChanged)
    Q_PROPERTY(QString importErrorMessage READ importErrorMessage NOTIFY importUiChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY stateChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(bool cacheAvailable READ cacheAvailable NOTIFY stateChanged)
    Q_PROPERTY(QString cacheStatusText READ cacheStatusText NOTIFY stateChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY workflowChanged)
    Q_PROPERTY(bool canRecheck READ canRecheck NOTIFY workflowChanged)
    Q_PROPERTY(bool canEditCell READ canEditCell NOTIFY operationChanged)
    Q_PROPERTY(QString cellDraft READ cellDraft NOTIFY cellInputSyncRequested)
    Q_PROPERTY(bool hasDraft READ hasDraft NOTIFY draftChanged)
    Q_PROPERTY(QString workflowPrompt READ workflowPrompt NOTIFY workflowChanged)
    Q_PROPERTY(QString workflowToken READ workflowToken NOTIFY workflowChanged)
    Q_PROPERTY(bool workflowPending READ workflowPending NOTIFY workflowChanged)
    Q_PROPERTY(QString fileMessage READ fileMessage NOTIFY workflowChanged)
    Q_PROPERTY(QVariantList recoveryCandidates READ recoveryCandidates NOTIFY recoveryCandidatesChanged)
    Q_PROPERTY(bool canDiscoverRecovery READ canDiscoverRecovery NOTIFY workflowChanged)
    Q_PROPERTY(bool recoveryRequiresSaveAs READ recoveryRequiresSaveAs NOTIFY stateChanged)

public:
    explicit ProjectViewModel(ProjectController &controller, QObject *parent = nullptr);
    ~ProjectViewModel() override;

    bool hasProject() const noexcept;
    double cell() const noexcept;
    QString cellText() const;
    QString runtimeIdentity() const;
    QString inputRevision() const;
    QString densityMapStatusText() const;
    bool canUndo() const noexcept { return controller_.canUndo(); }
    bool canRedo() const noexcept { return controller_.canRedo(); }
    QString errorMessage() const { return errorMessage_; }
    bool actionsEnabled() const noexcept { return controller_.shutdownState() == ShutdownState::Open; }
    bool shutdownReady() const noexcept { return controller_.shutdownState() == ShutdownState::Ready; }
    bool closing() const noexcept { return !actionsEnabled(); }
    bool operationVisible() const noexcept;
    bool operationActive() const noexcept { return controller_.operation().active; }
    bool canCancelOperation() const noexcept;
    bool progressKnown() const noexcept;
    double progressValue() const noexcept;
    QString operationText() const;
    QString phaseText() const;
    QString progressText() const;
    bool canImport() const noexcept;
    QString selectionToken() const { return selectionToken_; }
    bool selectingFile() const noexcept { return selectionContext_.has_value() && selectedPath_.isEmpty(); }
    bool replacementPending() const noexcept { return !selectedPath_.isEmpty() && !pendingImport_ && intent_ != Intent::Import; }
    QString replacementFileName() const;
    QString nonZeroZToken() const;
    QString nonZeroZText() const;
    bool hasDataset() const noexcept;
    QString sourceName() const;
    QString pointCountText() const;
    QString boundsText() const;
    bool datasetHasNonZeroZ() const noexcept;
    QString diagnosticText() const;
    QVariantMap importDiagnostic() const;
    QString importErrorMessage() const { return importErrorMessage_; }
    QString projectName() const;
    QString projectPath() const;
    bool dirty() const noexcept { return controller_.isDirty(); }
    bool cacheAvailable() const noexcept;
    QString cacheStatusText() const;
    bool canSave() const noexcept;
    bool canRecheck() const noexcept;
    bool canEditCell() const noexcept;
    QString cellDraft() const { return cellDraft_; }
    bool hasDraft() const { return hasProject() && cellDraft_ != cellText(); }
    QString workflowPrompt() const { return workflowPrompt_; }
    QString workflowToken() const { return workflowToken_; }
    bool workflowPending() const noexcept { return intent_ != Intent::None; }
    QString fileMessage() const { return fileMessage_; }
    QVariantList recoveryCandidates() const;
    bool canDiscoverRecovery() const noexcept;
    bool recoveryRequiresSaveAs() const noexcept { return controller_.recoveryRequiresSaveAs(); }

    Q_INVOKABLE void applyCell(const QString &text);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void newProject();
    Q_INVOKABLE void offerStartupImport();
    Q_INVOKABLE void selectImportFile(const QString &token, const QUrl &url);
    Q_INVOKABLE void cancelFileSelection(const QString &token);
    Q_INVOKABLE void confirmReplacement(const QString &token, bool confirmed);
    Q_INVOKABLE void answerNonZeroZ(const QString &token, bool proceed);
    Q_INVOKABLE void cancelOperation();
    Q_INVOKABLE bool requestClose();
    Q_INVOKABLE void editCell(const QString &text);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs();
    Q_INVOKABLE void open();
    Q_INVOKABLE void closeProject();
    Q_INVOKABLE void recheckPointCache();
    Q_INVOKABLE void showRecovery();
    Q_INVOKABLE void chooseRecovery(const QString &token, const QString &session, const QString &action);
    // Tokens are consumed before dispatch; Escape/closed always answers cancel.
    Q_INVOKABLE void answerWorkflow(const QString &token, const QString &answer);
    Q_INVOKABLE void selectProjectFile(const QString &token, const QUrl &url);

signals:
    void stateChanged();
    void errorMessageChanged();
    void cellInputSyncRequested();
    void operationChanged();
    void shutdownChanged();
    void importUiChanged();
    void fileSelectionRequested();
    void nonZeroZChanged();
    void draftChanged();
    void workflowChanged();
    void recoveryCandidatesChanged();

private:
    void handleResult(const CommandResult &result);
    void setErrorMessage(const QString &message);
    void clearSelection();
    void startSelectedImport(bool replacementConfirmed);
    void refreshImportUi();
    enum class Intent { None, Save, SaveAs, Open, Import, Close, Exit, Recheck, Recovery, RestoreRecovery, RejectRecovery };
    void beginWorkflow(Intent intent);
    void advanceWorkflow();
    void endWorkflow();
    void promptWorkflow(const QString &prompt);
    std::optional<OperationStamp> currentStamp() const;
    void observeFileOperation();
    void continueStartup();

    ProjectController &controller_;
    QString errorMessage_;
    std::optional<std::uint64_t> synchronizedIdentity_;
    double synchronizedCell_ = 0;
    std::optional<ImportContext> selectionContext_;
    QString selectionToken_;
    QString selectedPath_;
    std::optional<bool> pendingImport_; // one explicit user action waiting for Autosave's native join
    bool notifyingImportDispatch_ = false;
    bool submittingImport_ = false;
    QString importErrorMessage_;
    bool startupOffered_ = false;
    bool startupPending_ = false;
    bool zConsumerRegistered_ = false;
    QString cellDraft_;
    Intent intent_ = Intent::None;
    QString workflowPrompt_, workflowToken_, workflowPath_, savePath_, fileMessage_;
    std::optional<OperationStamp> workflowStamp_;
    std::optional<ImportContext> workflowContext_;
    QString approvedDraft_;
    bool discardApproved_ = false;
    bool saveRequested_ = false;
    bool advancingWorkflow_ = false;
    bool workflowRetry_ = false;
    bool waitingSave_ = false;
    OperationId workflowSaveId_ = 0;
    std::optional<bool> workflowSaveSucceeded_;
    OperationId observedFileOperation_ = 0;
    QString workflowInstance_;
    std::vector<persistence::RecoveryCandidate> recoveryCandidates_;
    std::optional<persistence::RecoveryCandidate> selectedRecovery_;
    bool recoveryReplacementApproved_ = false;
    OperationId recoveryOperationId_ = 0;
    std::optional<bool> recoveryOperationSucceeded_;
};

} // namespace cadcontour
