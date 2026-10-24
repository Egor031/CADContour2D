#include "project_view_model.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QTimer>

namespace cadcontour {

bool ProjectViewModel::canDiscoverRecovery() const noexcept
{
    return actionsEnabled() && !workflowPending() && !selectionContext_
        && (!operationActive() || controller_.operation().kind == OperationKind::Autosave);
}

void ProjectViewModel::showRecovery()
{
    if (canDiscoverRecovery()) beginWorkflow(Intent::Recovery);
}

QVariantList ProjectViewModel::recoveryCandidates() const
{
    QVariantList rows;
    using enum persistence::RecoverySessionState;
    for (const auto &candidate : recoveryCandidates_) {
        QString session, relation;
        switch (candidate.session) {
        case Active: session = QStringLiteral("Active — активный процесс, запись занята"); break;
        case Interrupted: session = QStringLiteral("Interrupted — прерванный сеанс"); break;
        case Closed: session = QStringLiteral("Closed — закрытый сеанс"); break;
        case Rejected: session = QStringLiteral("Rejected — явно отклонена"); break;
        case Invalid: session = QStringLiteral("Invalid — некорректная или неподдерживаемая запись"); break;
        }
        using enum persistence::RecoveryRelation;
        switch (candidate.relation) {
        case Unsaved: relation = QStringLiteral("Unsaved — без первого явного сохранения"); break;
        case MatchesExplicit: relation = QStringLiteral("MatchesExplicit — совпадает с явным сохранением"); break;
        case Modified: relation = QStringLiteral("Modified — есть несохранённые изменения"); break;
        case Conflict: relation = QStringLiteral("Conflict — явный проект независимо изменён или недоступен. После восстановления обязателен Save As в отдельный каталог."); break;
        case Unknown: relation = QStringLiteral("Unknown — отношение к явному сохранению неизвестно"); break;
        }
        QString metadata;
        if (candidate.record && candidate.record->state) {
            const auto &state = *candidate.record->state;
            metadata = QStringLiteral("Источник: %1\ncell: %2 мм · Точек: %3\nPoint Cache: %4\nДоступность кэша проверяется ограниченно при восстановлении.")
                .arg(QString::fromStdWString(state.content.sourcePath.wstring()))
                .arg(QString::number(state.snapshot.cell, 'g', QLocale::FloatingPointShortest))
                .arg(state.content.pointCount)
                .arg(QString::fromStdWString(state.content.cachePath().wstring()));
            metadata += QStringLiteral("\nSnapshot: ") + state.snapshot.snapshotId.toString(QUuid::WithoutBraces);
            if (candidate.record->baseline)
                metadata += QStringLiteral("\nЯвный проект: ") + candidate.record->baseline->manifestPath;
        }
        if (candidate.error)
            metadata += QStringLiteral("\nRecovery: код %1 · %2").arg(int(candidate.error->code)).arg(candidate.error->detail);
        const bool eligible = candidate.record && !candidate.error
            && (candidate.session == Interrupted || candidate.session == Closed);
        rows.push_back(QVariantMap{{"sessionId", candidate.sessionId.toString(QUuid::WithoutBraces)},
            {"description", session + u'\n' + relation + u'\n' + metadata}, {"eligible", eligible}});
    }
    return rows;
}

void ProjectViewModel::chooseRecovery(const QString &token, const QString &session, const QString &action)
{
    if (token.isEmpty() || token != workflowToken_ || workflowPrompt_ != QStringLiteral("recovery")) return;
    if (currentStamp() != workflowStamp_ || controller_.fileCommandContext() != workflowContext_) { endWorkflow(); return; }
    if (action != QStringLiteral("restore") && action != QStringLiteral("reject")) return;
    for (const auto &candidate : recoveryCandidates_) {
        if (candidate.sessionId.toString(QUuid::WithoutBraces) != session) continue;
        if (!candidate.record || candidate.error || (candidate.session != persistence::RecoverySessionState::Interrupted
            && candidate.session != persistence::RecoverySessionState::Closed)) return;
        selectedRecovery_ = candidate; // immutable metadata survives Autosave invalidation of discovery results
        intent_ = action == QStringLiteral("restore") ? Intent::RestoreRecovery : Intent::RejectRecovery;
        workflowPrompt_.clear(); workflowToken_.clear();
        if (intent_ == Intent::RejectRecovery) promptWorkflow(QStringLiteral("rejectRecovery"));
        else advanceWorkflow();
        return;
    }
}

QString ProjectViewModel::projectPath() const
{
    const auto *saved = controller_.savedProject();
    return saved ? saved->manifestPath : QString{};
}

QString ProjectViewModel::projectName() const
{
    return !projectPath().isEmpty() ? QFileInfo(projectPath()).completeBaseName()
        : hasProject() ? QStringLiteral("Без имени") : QStringLiteral("Нет открытого проекта");
}

bool ProjectViewModel::cacheAvailable() const noexcept
{
    const auto *availability = controller_.pointCacheAvailability();
    return availability && availability->access == persistence::CacheAccess::AvailableLimited;
}

QString ProjectViewModel::cacheStatusText() const
{
    const auto *availability = controller_.pointCacheAvailability();
    if (!availability) return {};
    QString reason;
    switch (availability->access) {
    case persistence::CacheAccess::AvailableLimited:
        return QStringLiteral("Point Cache доступен · Ограниченная проверка, без полной validation · Единицы: мм");
    case persistence::CacheAccess::Missing: reason = QStringLiteral("Файл Point Cache отсутствует"); break;
    case persistence::CacheAccess::Corrupt: reason = QStringLiteral("Point Cache повреждён"); break;
    case persistence::CacheAccess::Unsupported: reason = QStringLiteral("Формат Point Cache не поддерживается"); break;
    case persistence::CacheAccess::FileIO: reason = QStringLiteral("Ошибка доступа к Point Cache"); break;
    case persistence::CacheAccess::UnsafePath: reason = QStringLiteral("Небезопасный путь Point Cache"); break;
    }
    if (availability->pathError) reason += QStringLiteral("\n") + availability->pathError->detail;
    if (availability->cacheError) reason += QStringLiteral("\n") + availability->cacheError->detail;
    return reason + QStringLiteral("\nДоступны просмотр сведений, закрытие, замена и ограниченная повторная проверка. "
                                  "Изменение cell, Undo/Redo и сохранение запрещены до восстановления доступности.");
}

bool ProjectViewModel::canSave() const noexcept
{
    return actionsEnabled() && hasDataset() && cacheAvailable() && !controller_.unconfirmedSave()
        && !workflowPending() && !selectionContext_
        && (!operationActive() || controller_.operation().kind == OperationKind::Autosave);
}

bool ProjectViewModel::canRecheck() const noexcept
{
    return actionsEnabled() && hasProject() && (controller_.savedProject() || controller_.recoveryRecord())
        && !controller_.unconfirmedSave() && !workflowPending() && !selectionContext_
        && (!operationActive() || controller_.operation().kind == OperationKind::Autosave);
}

bool ProjectViewModel::canEditCell() const noexcept
{
    const auto &operation = controller_.operation();
    return actionsEnabled() && hasProject() && cacheAvailable()
        && (!operation.active || (operation.kind != OperationKind::Import && operation.kind != OperationKind::Open
                                 && operation.kind != OperationKind::RestoreRecovery));
}

std::optional<OperationStamp> ProjectViewModel::currentStamp() const
{
    return hasProject() ? std::optional(controller_.project()->operationStamp()) : std::nullopt;
}

void ProjectViewModel::editCell(const QString &text)
{
    if (cellDraft_ == text) return;
    cellDraft_ = text;
    emit draftChanged();
}

void ProjectViewModel::save() { if (canSave()) beginWorkflow(Intent::Save); }
void ProjectViewModel::saveAs() { if (canSave()) beginWorkflow(Intent::SaveAs); }
void ProjectViewModel::open() { beginWorkflow(Intent::Open); }
void ProjectViewModel::closeProject() { beginWorkflow(Intent::Close); }
void ProjectViewModel::recheckPointCache() { if (canRecheck()) beginWorkflow(Intent::Recheck); }

void ProjectViewModel::beginWorkflow(Intent intent)
{
    if (!actionsEnabled()) return;
    if (intent == Intent::Exit || intent == Intent::Close) startupPending_ = false;
    if (workflowPending()) {
        if (workflowSaveId_ && (intent == Intent::Exit || intent == Intent::Close)) intent_ = intent;
        else if (((intent_ == Intent::Import && pendingImport_) || intent_ == Intent::Recovery
                  || intent_ == Intent::RestoreRecovery || intent_ == Intent::RejectRecovery)
                 && (intent == Intent::Exit || intent == Intent::Close)) {
            const QPointer<ProjectViewModel> lifetime(this);
            endWorkflow();
            if (lifetime) beginWorkflow(intent);
        }
        return;
    }
    // Exit/Close supersede an unsubmitted file choice. The old callback loses its token.
    const QPointer<ProjectViewModel> lifetime(this);
    intent_ = intent;
    workflowInstance_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    workflowStamp_ = currentStamp();
    workflowContext_ = controller_.fileCommandContext();
    approvedDraft_ = cellDraft_;
    discardApproved_ = false;
    saveRequested_ = intent == Intent::Save || intent == Intent::SaveAs;
    workflowPath_.clear();
    savePath_.clear();
    fileMessage_.clear();
    selectedRecovery_.reset();
    recoveryReplacementApproved_ = false;
    // An already permitted Save must deliver its receipt before any close decision.
    if (operationActive() && controller_.operation().kind == OperationKind::Save)
        workflowSaveId_ = controller_.operation().id;
    if (intent != Intent::Import && selectionContext_) clearSelection();
    if (!lifetime) return;
    advanceWorkflow();
}

void ProjectViewModel::endWorkflow()
{
    const bool import = intent_ == Intent::Import;
    intent_ = Intent::None;
    workflowInstance_.clear();
    workflowPrompt_.clear();
    workflowToken_.clear();
    workflowPath_.clear();
    savePath_.clear();
    workflowSaveId_ = 0;
    waitingSave_ = false;
    workflowSaveSucceeded_.reset();
    recoveryOperationId_ = 0;
    recoveryOperationSucceeded_.reset();
    selectedRecovery_.reset();
    const QPointer<ProjectViewModel> lifetime(this);
    if (import) clearSelection();
    if (!lifetime) return;
    emit workflowChanged();
    if (lifetime && startupPending_) QTimer::singleShot(0, this, &ProjectViewModel::continueStartup);
}

void ProjectViewModel::promptWorkflow(const QString &prompt)
{
    workflowPrompt_ = prompt;
    workflowToken_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    approvedDraft_ = cellDraft_;
    emit workflowChanged();
}

void ProjectViewModel::answerWorkflow(const QString &token, const QString &answer)
{
    if (token.isEmpty() || token != workflowToken_) return;
    const auto prompt = workflowPrompt_;
    if (prompt == QStringLiteral("recovery")) {
        if (answer == QStringLiteral("refresh")) {
            if (currentStamp() != workflowStamp_ || controller_.fileCommandContext() != workflowContext_) { endWorkflow(); return; }
            intent_ = Intent::Recovery;
            workflowToken_.clear(); workflowPrompt_.clear();
            advanceWorkflow();
        } else if (answer == QStringLiteral("cancel")) endWorkflow();
        return;
    }
    if (prompt == QStringLiteral("replaceRecovery") || prompt == QStringLiteral("rejectRecovery")) {
        workflowToken_.clear(); workflowPrompt_.clear();
        if (answer != QStringLiteral("confirm") || currentStamp() != workflowStamp_
            || controller_.fileCommandContext() != workflowContext_) { endWorkflow(); return; }
        recoveryReplacementApproved_ = true;
        advanceWorkflow();
        return;
    }
    if (prompt != QStringLiteral("draft") && prompt != QStringLiteral("dirty")) {
        if (answer == QStringLiteral("cancel")) endWorkflow();
        return;
    }
    workflowToken_.clear();
    workflowPrompt_.clear();
    if (answer == QStringLiteral("cancel") || currentStamp() != workflowStamp_
        || controller_.fileCommandContext() != workflowContext_) {
        endWorkflow();
        return;
    }
    if (approvedDraft_ != cellDraft_) {
        if (intent_ == Intent::Import) promptWorkflow(QStringLiteral("draft"));
        else endWorkflow();
        return;
    }
    const QPointer<ProjectViewModel> lifetime(this);
    if (prompt == QStringLiteral("draft")) {
        if (answer == QStringLiteral("apply")) {
            // Suppress only our own Import selection invalidation during explicit Apply.
            const bool wasAdvancing = advancingWorkflow_;
            advancingWorkflow_ = true;
            applyCell(cellDraft_);
            if (!lifetime) return;
            advancingWorkflow_ = wasAdvancing;
            if (currentStamp() != workflowStamp_) discardApproved_ = false;
            workflowStamp_ = currentStamp();
            workflowContext_ = controller_.fileCommandContext();
            if (intent_ == Intent::Import) selectionContext_ = workflowContext_;
            if (hasDraft()) {
                if (intent_ == Intent::Import) promptWorkflow(QStringLiteral("draft"));
                else endWorkflow();
                return;
            }
        } else if (answer == QStringLiteral("discard")) {
            cellDraft_ = cellText();
            setErrorMessage({});
            if (!lifetime) return;
            emit cellInputSyncRequested();
            if (!lifetime) return;
            emit draftChanged();
            if (!lifetime) return;
        } else { endWorkflow(); return; }
    } else {
        if (answer == QStringLiteral("save")) saveRequested_ = true;
        else if (answer == QStringLiteral("discard")) discardApproved_ = true;
        else { endWorkflow(); return; }
    }
    advanceWorkflow();
}

void ProjectViewModel::selectProjectFile(const QString &token, const QUrl &url)
{
    if (token.isEmpty() || token != workflowToken_
        || (workflowPrompt_ != QStringLiteral("open") && workflowPrompt_ != QStringLiteral("save"))) return;
    if (currentStamp() != workflowStamp_ || approvedDraft_ != cellDraft_) { endWorkflow(); return; }
    const auto path = url.toLocalFile();
    if (!url.isValid() || !url.isLocalFile() || url.hasQuery() || url.hasFragment()
        || !url.userInfo().isEmpty() || url.port() != -1 || path.contains(QChar(u'\0'))
        || !QDir::isAbsolutePath(path)
        || QFileInfo(path).suffix().compare(QStringLiteral("pc2dproj"), Qt::CaseInsensitive) != 0) {
        fileMessage_ = QStringLiteral("Выберите локальный или сетевой файл .pc2dproj.");
        endWorkflow();
        return;
    }
    if (workflowPrompt_ == QStringLiteral("save")) savePath_ = path;
    else workflowPath_ = path;
    workflowToken_.clear();
    workflowPrompt_.clear();
    advanceWorkflow();
}

void ProjectViewModel::advanceWorkflow()
{
    if (!workflowPending()) return;
    if (advancingWorkflow_) { workflowRetry_ = true; return; }
    // Do not use a rollback holding a reference to this: observers can destroy the adapter.
    advancingWorkflow_ = true;
    const QPointer<ProjectViewModel> lifetime(this);
    const auto advance = [this, &lifetime] {
        if (!actionsEnabled()) { endWorkflow(); return; }
        if (recoveryOperationId_) {
            if (!recoveryOperationSucceeded_) return;
            const bool succeeded = *recoveryOperationSucceeded_;
            recoveryOperationId_ = 0; recoveryOperationSucceeded_.reset();
            if (intent_ == Intent::RestoreRecovery && succeeded) { endWorkflow(); return; }
            // Only our own completed metadata command may advance its admission context.
            if (currentStamp() != workflowStamp_ || controller_.fileCommandContext() != workflowContext_) {
                endWorkflow(); return;
            }
            if (intent_ == Intent::Recovery && succeeded && recoveryCandidates_.empty()) { endWorkflow(); return; }
            if (intent_ == Intent::RejectRecovery && succeeded) {
                intent_ = Intent::Recovery; // refresh lifecycle/digests after the persisted receipt
            } else {
                intent_ = Intent::Recovery;
                promptWorkflow(QStringLiteral("recovery")); return;
            }
        }
        if (workflowSaveId_) {
            if (!workflowSaveSucceeded_) return;
            const bool succeeded = *workflowSaveSucceeded_;
            workflowSaveId_ = 0;
            workflowSaveSucceeded_.reset();
            if (waitingSave_ && (!succeeded || intent_ == Intent::Save || intent_ == Intent::SaveAs)) {
                endWorkflow(); return;
            }
            waitingSave_ = false;
            workflowStamp_ = currentStamp();
            workflowContext_ = controller_.fileCommandContext();
            discardApproved_ = false;
            if (intent_ == Intent::Import) selectionContext_ = workflowContext_;
        }
        if (currentStamp() != workflowStamp_) {
            fileMessage_ = QStringLiteral("Состояние проекта изменилось. Повторите команду.");
            endWorkflow(); return;
        }
        const auto context = controller_.fileCommandContext();
        if (workflowContext_ && context && workflowContext_ != context) {
            fileMessage_ = QStringLiteral("Контекст команды изменился. Повторите команду.");
            endWorkflow(); return;
        }
        if (!workflowPrompt_.isEmpty()) return;
        if (intent_ == Intent::RestoreRecovery && hasProject() && !recoveryReplacementApproved_) {
            promptWorkflow(QStringLiteral("replaceRecovery")); return;
        }
        // Open selects its own file; cancelling it never discards the existing draft.
        if (intent_ == Intent::Open && workflowPath_.isEmpty()) { promptWorkflow(QStringLiteral("open")); return; }
        if (intent_ != Intent::Recheck && intent_ != Intent::Recovery && intent_ != Intent::RejectRecovery
            && hasDraft()) { promptWorkflow(QStringLiteral("draft")); return; }
        const bool replacing = intent_ == Intent::Open || intent_ == Intent::Import
            || intent_ == Intent::Close || intent_ == Intent::Exit || intent_ == Intent::RestoreRecovery;
        if (replacing && dirty() && !discardApproved_ && !saveRequested_) { promptWorkflow(QStringLiteral("dirty")); return; }
        if (saveRequested_) {
            if (!cacheAvailable() || controller_.unconfirmedSave()) {
                fileMessage_ = QStringLiteral("Сохранение недоступно. Проверьте Point Cache или откройте manifest после неопределённого Save.");
                endWorkflow(); return;
            }
            if ((intent_ == Intent::SaveAs || !controller_.savedProject() || controller_.recoveryRequiresSaveAs())
                && savePath_.isEmpty()) { promptWorkflow(QStringLiteral("save")); return; }
        }
        if (!saveRequested_ && intent_ == Intent::Import) {
            // Keep the workflow until actual admission: a deferred retry must
            // resolve any newer draft before using the retained confirmation.
            startSelectedImport(true);
            return;
        }
        if (operationActive()) {
            // Close/Exit drain other work through the existing shutdown guards. Save waits for its receipt above.
            if (saveRequested_ || (intent_ != Intent::Exit && intent_ != Intent::Close)) {
                if (controller_.operation().kind == OperationKind::Autosave) controller_.cancelOperation();
                return; // only commandTransitionFinished retries; no timer polling
            }
        }
        if (saveRequested_) {
            SaveRequest request;
            // For Open, workflowPath_ is the incoming document, never a Save destination.
            request.manifestPath = savePath_;
            const bool saveAs = intent_ == Intent::SaveAs || controller_.recoveryRequiresSaveAs();
            const auto result = saveAs ? controller_.saveProjectAs(request) : controller_.saveProject(request);
            if (!lifetime) return;
            if (result.outcome == CommandOutcome::Rejected) {
                if (result.failure->category == CommandError::Busy || result.failure->detail == ErrorDetail::ReentrantCommand) return;
                handleResult(result); if (lifetime) endWorkflow(); return;
            }
            saveRequested_ = false;
            waitingSave_ = true;
            workflowSaveId_ = controller_.operation().id;
            observeFileOperation();
            return;
        }
        CommandResult result = CommandResult::noChange();
        if (intent_ == Intent::Recovery || intent_ == Intent::RestoreRecovery || intent_ == Intent::RejectRecovery) {
            const auto instance = workflowInstance_;
            if (intent_ == Intent::Recovery) result = controller_.discoverRecovery();
            else if (intent_ == Intent::RestoreRecovery) {
                RecoveryRequest request{*selectedRecovery_};
                request.replacementConfirmed = hasProject(); request.expectedContext = workflowContext_;
                result = controller_.restoreRecovery(std::move(request));
            } else result = controller_.rejectRecovery(*selectedRecovery_);
            if (!lifetime || instance != workflowInstance_) return;
            if (result.outcome == CommandOutcome::Applied) {
                recoveryOperationId_ = controller_.operation().id;
                workflowContext_ = controller_.fileCommandContext();
                observeFileOperation(); return;
            }
        }
        else if (intent_ == Intent::Open) {
            OpenRequest request;
            request.manifestPath = workflowPath_;
            request.replacementConfirmed = hasProject();
            request.expectedContext = workflowContext_;
            result = controller_.openProject(std::move(request));
        } else if (intent_ == Intent::Close) result = controller_.closeProject(discardApproved_);
        else if (intent_ == Intent::Exit) result = controller_.requestShutdown(discardApproved_);
        else if (intent_ == Intent::Recheck) result = controller_.recheckPointCache();
        if (!lifetime) return;
        if (result.outcome == CommandOutcome::Rejected) {
            if (result.failure->category == CommandError::Busy || result.failure->detail == ErrorDetail::ReentrantCommand) return;
            handleResult(result);
            if (!lifetime) return;
        }
        endWorkflow();
    };
    advance();
    if (lifetime) {
        advancingWorkflow_ = false;
        if (std::exchange(workflowRetry_, false))
            QTimer::singleShot(0, this, &ProjectViewModel::advanceWorkflow);
        emit workflowChanged();
    }
}

void ProjectViewModel::observeFileOperation()
{
    const QPointer<ProjectViewModel> lifetime(this);
    const auto &operation = controller_.operation();
    if (recoveryOperationId_ == operation.id && !operation.active && !recoveryOperationSucceeded_) {
        recoveryOperationSucceeded_ = operation.status == OperationStatus::Succeeded;
        if (*recoveryOperationSucceeded_ && operation.kind == OperationKind::DiscoverRecovery) {
            recoveryCandidates_ = controller_.recoveryCandidates();
            emit recoveryCandidatesChanged();
            if (!lifetime) return;
        }
    }
    if (workflowSaveId_ == operation.id && !operation.active)
        workflowSaveSucceeded_ = operation.status == OperationStatus::Succeeded && !controller_.unconfirmedSave();
    if (!operation.active && operation.id && observedFileOperation_ != operation.id
        && (operation.kind == OperationKind::Save || operation.kind == OperationKind::Open
            || operation.kind == OperationKind::RecheckCache || operation.kind == OperationKind::DiscoverRecovery
            || operation.kind == OperationKind::RestoreRecovery || operation.kind == OperationKind::RejectRecovery)) {
        observedFileOperation_ = operation.id;
        if (operation.status == OperationStatus::Succeeded)
            fileMessage_ = operation.kind == OperationKind::Save
                ? dirty() ? QStringLiteral("Snapshot сохранён; остались более поздние несохранённые изменения.")
                          : QStringLiteral("Проект сохранён.")
                : operation.kind == OperationKind::Open ? QStringLiteral("Проект открыт.")
                : operation.kind == OperationKind::RestoreRecovery ? QStringLiteral("Проект восстановлен. История Undo/Redo пуста.")
                : operation.kind == OperationKind::RejectRecovery ? QStringLiteral("Запись восстановления явно отклонена.")
                : operation.kind == OperationKind::DiscoverRecovery ? QStringLiteral("Поиск записей восстановления завершён.")
                : QStringLiteral("Ограниченная проверка Point Cache завершена.");
        else if (operation.status == OperationStatus::Cancelled || operation.status == OperationStatus::Discarded)
            fileMessage_ = QStringLiteral("Операция отменена или результат отклонён. Текущий проект сохранён в сеансе.");
        else {
            fileMessage_ = QStringLiteral("Ошибка файловой операции. Текущий проект и история сохранены.");
            if (operation.failure && operation.failure->saveError) {
                const auto &error = *operation.failure->saveError;
                using enum persistence::SaveErrorCode;
                switch (error.code) {
                case InvalidTarget: fileMessage_ += QStringLiteral("\nНедопустимое расположение проекта."); break;
                case Conflict: fileMessage_ += QStringLiteral("\nКонфликт пути или независимо изменённый manifest. Выберите отдельный каталог для Save As."); break;
                case Locked: fileMessage_ += QStringLiteral("\nКаталог занят другой операцией сохранения."); break;
                case MissingDataset: fileMessage_ += QStringLiteral("\nPoint Cache отсутствует."); break;
                case ChangedDataset: fileMessage_ += QStringLiteral("\nPoint Cache изменился после проверки."); break;
                case FileIO: fileMessage_ += QStringLiteral("\nОшибка чтения или записи файлов."); break;
                case ResourceUnavailable: fileMessage_ += QStringLiteral("\nНедостаточно доступных ресурсов."); break;
                case InvalidSnapshot: fileMessage_ += QStringLiteral("\nНекорректное сохраняемое состояние."); break;
                case Cancelled: fileMessage_ += QStringLiteral("\nСохранение отменено."); break;
                case PublicationUncertain: break;
                }
                fileMessage_ += QStringLiteral("\nSave: код %1, фаза %2, системный код %3: %4")
                    .arg(int(error.code)).arg(int(error.phase)).arg(error.systemCode)
                    .arg(QString::fromUtf8(error.detail.data()));
            }
            if (operation.failure && operation.failure->openError)
                fileMessage_ += QStringLiteral("\n") + operation.failure->openError->detail;
            if (operation.failure && operation.failure->recoveryError)
                fileMessage_ += QStringLiteral("\nRecovery: код %1 · %2")
                    .arg(int(operation.failure->recoveryError->code)).arg(operation.failure->recoveryError->detail);
            if (controller_.unconfirmedSave())
                fileMessage_ += QStringLiteral("\nРезультат Save неопределён. Для проверки откройте manifest заново; повторное сохранение запрещено.");
        }
    }
    emit workflowChanged();
}

} // namespace cadcontour
