#include "project_view_model.h"

#include <QLocale>
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace cadcontour {
namespace {
QString importFailureText(const cache::Error &error)
{
    using enum cache::ErrorCode;
    QString reason;
    switch (error.code) {
    case OpenFailed: reason = QStringLiteral("Не удалось открыть исходный файл или хранилище кэша"); break;
    case ReadFailed: reason = QStringLiteral("Ошибка чтения исходного файла или кэша"); break;
    case ParseError: reason = QStringLiteral("Некорректная запись ASC/XYZ"); break;
    case NumericRange: reason = QStringLiteral("Координата выходит за диапазон binary64"); break;
    case InsufficientData: reason = QStringLiteral("Файл не содержит измерений"); break;
    case SourceChanged: reason = QStringLiteral("Исходный файл изменился во время импорта"); break;
    case ResourceExhausted: reason = QStringLiteral("Недостаточно памяти, места на диске или доступных файловых ресурсов"); break;
    case WriteFailed: case FlushFailed: case PublishFailed:
        reason = QStringLiteral("Не удалось записать или опубликовать Point Cache"); break;
    case Cancelled: reason = QStringLiteral("Импорт отменён"); break;
    default: reason = QStringLiteral("Ошибка импорта или построения Point Cache"); break;
    }
    if (error.source)
        reason += QStringLiteral(". Строка %1, столбец %2").arg(error.source->line).arg(error.source->column);
    return reason + QStringLiteral(". Результат не применён.");
}

QString codeName(cache::ErrorCode value)
{
#define CASE(name) case cache::ErrorCode::name: return QStringLiteral(#name)
    switch (value) {
    CASE(InvalidInput); CASE(UnsupportedFormat); CASE(CorruptCache); CASE(IncompleteCache);
    CASE(OpenFailed); CASE(ReadFailed); CASE(WriteFailed); CASE(SeekFailed); CASE(FlushFailed);
    CASE(PublishFailed); CASE(ResourceExhausted); CASE(Cancelled); CASE(InvalidState);
    CASE(ConsumerFailed); CASE(CleanupFailed); CASE(ParseError); CASE(NumericRange);
    CASE(InsufficientData); CASE(SourceChanged);
    }
#undef CASE
    return {};
}
QString phaseName(cache::Phase value)
{
#define CASE(name) case cache::Phase::name: return QStringLiteral(#name)
    switch (value) {
    CASE(Codec); CASE(Layout); CASE(FileIO); CASE(Publication); CASE(Ingest); CASE(RunSort);
    CASE(RunWrite); CASE(Merge); CASE(Output); CASE(Cleanup); CASE(PointPages); CASE(IndexBuild);
    CASE(Sealing); CASE(CacheRead); CASE(Validation); CASE(SourceRead); CASE(Parse);
    }
#undef CASE
    return {};
}
QString operationName(cache::Operation value)
{
#define CASE(name) case cache::Operation::name: return QStringLiteral(#name)
    switch (value) {
    CASE(Encode); CASE(Decode); CASE(Calculate); CASE(Open); CASE(Read); CASE(Write); CASE(Seek);
    CASE(Size); CASE(Flush); CASE(Publish); CASE(Append); CASE(Sort); CASE(Consume); CASE(Remove);
    }
#undef CASE
    return {};
}
}

ProjectViewModel::ProjectViewModel(ProjectController &controller, QObject *parent)
    : QObject(parent), controller_(controller)
{
    zConsumerRegistered_ = controller_.setNonZeroZConsumer(this).outcome != CommandOutcome::Rejected;
    if (const auto *state = controller_.project()) {
        synchronizedIdentity_ = state->runtimeIdentity();
        synchronizedCell_ = state->cell();
    }
    cellDraft_ = cellText();
    connect(this, &ProjectViewModel::workflowChanged, this, &ProjectViewModel::importUiChanged);
    connect(&controller_, &ProjectController::stateChanged, this, [this] {
        const QPointer<ProjectViewModel> lifetime(this);
        const auto *state = controller_.project();
        const auto identity = state ? std::optional(state->runtimeIdentity()) : std::nullopt;
        const double value = state ? state->cell() : 0;
        const bool sync = identity != synchronizedIdentity_ || value != synchronizedCell_;
        synchronizedIdentity_ = identity;
        synchronizedCell_ = value;
        if (sync) {
            cellDraft_ = cellText();
            setErrorMessage({});
        }
        if (!lifetime) return;
        emit stateChanged();
        if (!lifetime) return;
        if (sync)
            emit cellInputSyncRequested();
        if (!lifetime) return;
        emit draftChanged();
        if (!lifetime) return;
        emit workflowChanged();
        if (!lifetime) return;
        emit operationChanged(); // visibility/text also depend on the current project identity
        if (!lifetime) return;
        refreshImportUi();
    });
    connect(&controller_, &ProjectController::operationChanged, this, &ProjectViewModel::operationChanged);
    connect(&controller_, &ProjectController::shutdownChanged, this, &ProjectViewModel::shutdownChanged);
    connect(&controller_, &ProjectController::operationChanged, this, &ProjectViewModel::refreshImportUi);
    connect(&controller_, &ProjectController::shutdownChanged, this, &ProjectViewModel::refreshImportUi);
    connect(&controller_, &ProjectController::operationChanged, this, &ProjectViewModel::observeFileOperation);
    connect(&controller_, &ProjectController::commandTransitionFinished, this,
        [this, controller = QPointer<ProjectController>(&controller)] {
            if (!controller) return;
            const QPointer<ProjectViewModel> lifetime(this);
            if (pendingImport_) startSelectedImport(*pendingImport_);
            if (lifetime) advanceWorkflow();
        }, Qt::QueuedConnection);
    connect(&controller_, &ProjectController::nonZeroZConfirmationChanged, this, [this] {
        const QPointer<ProjectViewModel> lifetime(this);
        emit nonZeroZChanged();
        if (!lifetime) return;
        emit operationChanged();
    });
}

ProjectViewModel::~ProjectViewModel()
{
    // QObject::destroyed revokes this consumer and may synchronously emit controller
    // notifications. Stop callbacks before the derived object ceases to exist.
    disconnect(&controller_, nullptr, this, nullptr);
}

bool ProjectViewModel::hasProject() const noexcept { return controller_.project() != nullptr; }
double ProjectViewModel::cell() const noexcept
{
    const auto *state = controller_.project();
    return state ? state->cell() : 0.0;
}

QString ProjectViewModel::cellText() const
{
    return hasProject() ? QLocale::c().toString(cell(), 'g', QLocale::FloatingPointShortest) : QString{};
}

QString ProjectViewModel::runtimeIdentity() const
{
    const auto *state = controller_.project();
    return state ? QString::number(state->runtimeIdentity()) : QString{};
}

QString ProjectViewModel::inputRevision() const
{
    const auto *state = controller_.project();
    return state ? QString::number(state->inputRevision()) : QString{};
}

QString ProjectViewModel::densityMapStatusText() const
{
    return hasProject() ? QStringLiteral("Карта плотности отсутствует")
                        : QStringLiteral("Нет открытого проекта");
}

void ProjectViewModel::applyCell(const QString &text)
{
    QString normalized = text.trimmed();
    const QRegularExpression number(QStringLiteral(
        "\\A[+-]?(?:[0-9]+(?:[.,][0-9]*)?|[.,][0-9]+)(?:[eE][+-]?[0-9]+)?\\z"));
    if (!number.match(normalized).hasMatch()) {
        setErrorMessage(QStringLiteral("Введите положительное число: например, 1 или 0,5. "
                                       "Используйте одну десятичную точку или запятую без разделителей групп."));
        return;
    }
    normalized.replace(u',', u'.');
    bool ok = false;
    const double value = normalized.toDouble(&ok);
    if (!ok) {
        setErrorMessage(QStringLiteral("Число выходит за диапазон double. Введите конечное значение больше нуля."));
        return;
    }
    const QPointer<ProjectViewModel> lifetime(this);
    const auto result = controller_.setCell(value);
    if (!lifetime) return;
    handleResult(result);
    if (!lifetime) return;
    if (result.outcome == CommandOutcome::NoChange) {
        cellDraft_ = cellText();
        setErrorMessage({});
        if (!lifetime) return;
        emit cellInputSyncRequested(); // only successful Apply canonicalizes a no-op draft
        if (lifetime) emit draftChanged();
    }
}

void ProjectViewModel::undo()
{
    const QPointer<ProjectViewModel> lifetime(this);
    const auto result = controller_.undo();
    if (lifetime) handleResult(result);
}
void ProjectViewModel::redo()
{
    const QPointer<ProjectViewModel> lifetime(this);
    const auto result = controller_.redo();
    if (lifetime) handleResult(result);
}
bool ProjectViewModel::canImport() const noexcept
{
    return actionsEnabled() && (!operationActive() || controller_.operation().kind == OperationKind::Autosave)
        && !selectionContext_ && !workflowPending() && zConsumerRegistered_;
}

void ProjectViewModel::offerStartupImport()
{
    if (std::exchange(startupOffered_, true) || hasProject())
        return;
    newProject();
}

void ProjectViewModel::newProject()
{
    if (!canImport()) {
        importErrorMessage_ = closing() ? QStringLiteral("Приложение завершает работу.")
            : QStringLiteral("Дождитесь завершения предыдущей операции или выбора файла.");
        emit importUiChanged();
        return;
    }
    selectionContext_ = controller_.importSelectionContext();
    if (!selectionContext_) {
        importErrorMessage_ = QStringLiteral("Сейчас невозможно начать импорт. Повторите выбор файла.");
        emit importUiChanged();
        return;
    }
    const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    selectionToken_ = token;
    importErrorMessage_.clear();
    const QPointer<ProjectViewModel> lifetime(this);
    emit importUiChanged();
    if (lifetime && selectionToken_ == token && actionsEnabled() && selectingFile()
        && selectionContext_ == controller_.importSelectionContext())
        emit fileSelectionRequested();
}

void ProjectViewModel::clearSelection()
{
    selectionContext_.reset();
    selectionToken_.clear();
    selectedPath_.clear();
    pendingImport_.reset();
    emit importUiChanged();
}

void ProjectViewModel::refreshImportUi()
{
    if (intent_ == Intent::Import && !advancingWorkflow_ && !waitingSave_
        && currentStamp() != workflowStamp_) {
        endWorkflow();
        return;
    }
    const bool ownAdmission = submittingImport_ && !closing()
        && controller_.operation().kind == OperationKind::Import;
    const bool ownWorkflow = intent_ == Intent::Import;
    if (selectionContext_ && !ownAdmission && !ownWorkflow && selectionContext_ != controller_.importSelectionContext()) {
        importErrorMessage_ = closing() ? QStringLiteral("Приложение завершает работу.")
            : QStringLiteral("Контекст изменился. Выберите файл заново для нового импорта.");
        clearSelection();
        return;
    }
    emit importUiChanged();
}

void ProjectViewModel::cancelFileSelection(const QString &token)
{
    if (!token.isEmpty() && token == selectionToken_) {
        if (intent_ == Intent::Import) endWorkflow();
        else clearSelection();
    }
}

void ProjectViewModel::selectImportFile(const QString &token, const QUrl &url)
{
    if (token.isEmpty() || token != selectionToken_ || !selectingFile())
        return;
    // QUrl decodes percent escapes exactly once and preserves Windows/UNC semantics.
    const QString path = url.toLocalFile();
    if (!url.isValid() || !url.isLocalFile() || url.hasQuery() || url.hasFragment()
        || !url.userInfo().isEmpty() || url.port() != -1 || path.contains(QChar(u'\0'))
        || !QDir::isAbsolutePath(path)
        || (QFileInfo(path).suffix().compare(QStringLiteral("asc"), Qt::CaseInsensitive) != 0
            && QFileInfo(path).suffix().compare(QStringLiteral("xyz"), Qt::CaseInsensitive) != 0)) {
        importErrorMessage_ = QStringLiteral("Выберите локальный или сетевой файл ASC/XYZ.");
        clearSelection();
        return;
    }
    if (selectionContext_ != controller_.importSelectionContext()) {
        importErrorMessage_ = QStringLiteral("Контекст изменился. Выберите файл заново.");
        clearSelection();
        return;
    }
    selectedPath_ = path;
    if (hasProject())
        emit importUiChanged();
    else
        startSelectedImport(false);
}

QString ProjectViewModel::replacementFileName() const { return QFileInfo(selectedPath_).fileName(); }

void ProjectViewModel::confirmReplacement(const QString &token, bool confirmed)
{
    if (token.isEmpty() || token != selectionToken_ || !replacementPending())
        return;
    if (confirmed)
        beginWorkflow(Intent::Import);
    else
        clearSelection();
}

void ProjectViewModel::startSelectedImport(bool confirmed)
{
    if (notifyingImportDispatch_ || submittingImport_) return;
    const auto token = selectionToken_;
    const bool notify = !pendingImport_;
    pendingImport_ = confirmed;
    const QPointer<ProjectViewModel> lifetime(this);
    if (notify) {
        // Close the presentation before dispatch, retaining intent until admission.
        // A synchronous observer may cancel, invalidate or destroy this adapter.
        notifyingImportDispatch_ = true;
        emit importUiChanged();
        if (!lifetime) return;
        notifyingImportDispatch_ = false;
        if (selectionToken_ != token || !pendingImport_) return;
    }
    if (!selectionContext_ || selectionContext_ != controller_.importSelectionContext()) {
        importErrorMessage_ = QStringLiteral("Контекст изменился. Выберите файл заново.");
        clearSelection();
        return;
    }
    if (!controller_.importContext()) {
        if (operationActive() && controller_.operation().kind == OperationKind::Autosave)
            controller_.cancelOperation(); // before Accept: revoke; after Accept: wait for receipt/join
        // No self-posting retry. The next completed command transition wakes us.
        return;
    }
    ImportRequest request{selectedPath_, {}, confirmed, selectionContext_, true};
    submittingImport_ = true;
    const auto result = controller_.importAscXyz(std::move(request));
    if (!lifetime) return;
    submittingImport_ = false;
    if (result.outcome == CommandOutcome::Rejected
        && (result.failure->category == CommandError::Busy || result.failure->detail == ErrorDetail::ReentrantCommand)
        && selectionContext_ && selectionContext_ == controller_.importSelectionContext())
        return;
    if (selectionToken_ == token) clearSelection();
    if (!lifetime) return;
    if (result.outcome == CommandOutcome::Rejected) {
        importErrorMessage_ = closing() ? QStringLiteral("Приложение завершает работу.")
            : result.failure->category == CommandError::Busy
                ? QStringLiteral("Дождитесь завершения предыдущей операции.")
                : QStringLiteral("Импорт не запущен: контекст или разрешение замены недействительны. Выберите файл заново.");
        emit importUiChanged();
    }
}

QString ProjectViewModel::nonZeroZToken() const
{
    const auto request = controller_.pendingNonZeroZ();
    return request ? request->requestId.toString(QUuid::WithoutBraces) : QString{};
}

QString ProjectViewModel::nonZeroZText() const
{
    const auto request = controller_.pendingNonZeroZ();
    if (!request) return {};
    return QStringLiteral("В строке %1 обнаружена ненулевая координата Z (%2). Ожидается плоское облако точек.\n\n"
        "При продолжении импорт будет выполнен с использованием координат X и Y для плоской обработки. Продолжить импорт?")
        .arg(request->line).arg(QLocale::c().toString(request->value, 'g', QLocale::FloatingPointShortest));
}

void ProjectViewModel::answerNonZeroZ(const QString &token, bool proceed)
{
    const auto request = controller_.pendingNonZeroZ();
    if (request && !token.isEmpty() && token == request->requestId.toString(QUuid::WithoutBraces))
        controller_.answerNonZeroZ(*request, proceed ? io::ZDecision::Continue : io::ZDecision::Cancel);
}

bool ProjectViewModel::hasDataset() const noexcept
{
    return hasProject() && controller_.project()->dataset() != nullptr;
}
QString ProjectViewModel::sourceName() const
{
    return hasDataset() ? QFileInfo(QString::fromStdWString(controller_.project()->dataset()->sourcePath.wstring())).fileName()
                        : QString{};
}
QString ProjectViewModel::pointCountText() const
{
    return hasDataset() ? QString::number(controller_.project()->dataset()->pointCount) : QString{};
}
QString ProjectViewModel::boundsText() const
{
    if (!hasDataset()) return {};
    const auto &data = *controller_.project()->dataset();
    const auto number = [](double value) {
        return QLocale::c().toString(value, 'g', QLocale::FloatingPointShortest);
    };
    return QStringLiteral("X: %1 … %2 мм\nY: %3 … %4 мм")
        .arg(number(data.minimum.x), number(data.maximum.x), number(data.minimum.y), number(data.maximum.y));
}
bool ProjectViewModel::datasetHasNonZeroZ() const noexcept
{
    return hasDataset() && controller_.project()->dataset()->hasNonZeroZ;
}

void ProjectViewModel::handleResult(const CommandResult &result)
{
    if (result.outcome != CommandOutcome::Rejected) {
        return;
    }
    const auto &failure = *result.failure;
    switch (failure.category) {
    case CommandError::ResourceUnavailable:
        setErrorMessage(QStringLiteral("Недостаточно памяти для подготовки операции."));
        break;
    case CommandError::InvalidValue:
        setErrorMessage(QStringLiteral("Размер ячейки должен быть конечным числом строго больше нуля."));
        break;
    case CommandError::NoProject:
        setErrorMessage(QStringLiteral("Нет открытого проекта. Создайте новый проект."));
        break;
    case CommandError::UnsafeState:
        if (failure.detail == ErrorDetail::ImportRequired)
            setErrorMessage(QStringLiteral("Новый проект создаётся после импорта ASC/XYZ."));
        else if (failure.detail == ErrorDetail::ShuttingDown)
            setErrorMessage(QStringLiteral("Приложение завершает работу."));
        else if (failure.detail == ErrorDetail::WrongThread)
            setErrorMessage(QStringLiteral("Команда вызвана из недопустимого потока."));
        else if (failure.detail == ErrorDetail::DatasetUnavailable)
            setErrorMessage(QStringLiteral("Point Cache недоступен. Восстановите файл и повторите ограниченную проверку."));
        else
            setErrorMessage(failure.detail == ErrorDetail::ReentrantCommand
                ? QStringLiteral("Предыдущая команда ещё не завершена.")
                : QStringLiteral("Невозможно безопасно изменить состояние: исчерпан внутренний счётчик."));
        break;
    case CommandError::Busy:
        setErrorMessage(QStringLiteral("Дождитесь завершения предыдущей операции."));
        break;
    }
}

void ProjectViewModel::cancelOperation()
{
    const QPointer<ProjectViewModel> lifetime(this);
    const auto result = controller_.cancelOperation();
    if (lifetime) handleResult(result);
}
bool ProjectViewModel::requestClose()
{
    if (shutdownReady()) return true;
    beginWorkflow(Intent::Exit);
    return false; // closure is retried after the settled shutdown notification
}

bool ProjectViewModel::operationVisible() const noexcept
{
    const auto &operation = controller_.operation();
    return operation.active || (operation.kind == OperationKind::Import && operation.id != 0)
        || (hasProject() && operation.id != 0
        && operation.stamp.projectIdentity == controller_.project()->runtimeIdentity());
}
bool ProjectViewModel::canCancelOperation() const noexcept
{
    const auto &operation = controller_.operation();
    return actionsEnabled() && operation.active && !operation.cancellationRequested
        && !operation.publicationRevoked && operation.decision == PublicationDecision::Pending;
}
bool ProjectViewModel::progressKnown() const noexcept
{
    const auto &total = controller_.operation().progress.total;
    return total && *total > 0;
}
double ProjectViewModel::progressValue() const noexcept
{
    const auto &progress = controller_.operation().progress;
    return progressKnown() ? std::min(1.0, double(progress.completed) / double(*progress.total)) : 0;
}
QString ProjectViewModel::phaseText() const
{
    if (!nonZeroZToken().isEmpty()) return QStringLiteral("Ожидается подтверждение ненулевого Z");
    switch (controller_.operation().progress.phase) {
    case OperationPhase::Preparing: return QStringLiteral("Подготовка");
    case OperationPhase::Computing: return QStringLiteral("Вычисление");
    case OperationPhase::Parsing: return QStringLiteral("Разбор ASC/XYZ и подготовка сортируемых блоков");
    case OperationPhase::BuildingCache: return QStringLiteral("Построение Point Cache: сортировка, индекс и проверка");
    case OperationPhase::Finalizing: return QStringLiteral("Проверка результата");
    case OperationPhase::Cleanup: return QStringLiteral("Освобождение ресурсов");
    }
    return {};
}
QString ProjectViewModel::progressText() const
{
    const auto &progress = controller_.operation().progress;
    if (controller_.operation().kind == OperationKind::Import && !progressKnown())
        return QStringLiteral("Общий объём фазы неизвестен");
    if (!progressKnown())
        return QStringLiteral("Обработано: %1 · Общий объём неизвестен").arg(progress.completed);
    return QStringLiteral("%1 / %2 · %3 %").arg(progress.completed).arg(*progress.total)
        .arg(100 * progressValue(), 0, 'f', 1);
}
QString ProjectViewModel::operationText() const
{
    const auto &operation = controller_.operation();
    const auto name = [&] {
        switch (operation.kind) {
        case OperationKind::Import: return QStringLiteral("Импорт ASC/XYZ");
        case OperationKind::Save: return QStringLiteral("Сохранение проекта");
        case OperationKind::Open: return QStringLiteral("Открытие проекта");
        case OperationKind::RecheckCache: return QStringLiteral("Ограниченная проверка Point Cache");
        case OperationKind::Autosave: return QStringLiteral("Автосохранение");
        default: return QStringLiteral("Фоновая операция");
        }
    };
    if (closing())
        return QStringLiteral("Завершение приложения: ожидание остановки операции…");
    if (operation.kind != OperationKind::Import && operation.active && (!hasProject()
        || operation.stamp.projectIdentity != controller_.project()->runtimeIdentity()))
        return QStringLiteral("Завершение операции предыдущего проекта…");
    switch (operation.status) {
    case OperationStatus::Idle: return {};
    case OperationStatus::Starting: return QStringLiteral("Запуск фоновой операции…");
    case OperationStatus::Running:
        return operation.publicationRevoked ? QStringLiteral("Входы изменились: ожидание завершения…")
                                            : name();
    case OperationStatus::Cancelling: return QStringLiteral("Остановка операции…");
    case OperationStatus::Finishing: return QStringLiteral("Завершение операции…");
    case OperationStatus::Succeeded: return operation.kind == OperationKind::Import
        ? QStringLiteral("Импорт завершён. Point Cache готов.") : QStringLiteral("Операция завершена.");
    case OperationStatus::Cancelled: return QStringLiteral("Операция отменена. Результат не применён.");
    case OperationStatus::Discarded: return QStringLiteral("Входные данные изменились. Результат не применён.");
    case OperationStatus::Failed: {
        if (operation.failure && operation.failure->importError)
            return importFailureText(*operation.failure->importError);
        QString reason = QStringLiteral("Ошибка выполнения");
        if (operation.failure) {
            switch (operation.failure->reason) {
            case FailureReason::OutOfMemory: reason = QStringLiteral("Недостаточно памяти"); break;
            case FailureReason::LaunchFailed: reason = QStringLiteral("Не удалось запустить поток"); break;
            case FailureReason::NativeObservationFailed: reason = QStringLiteral("Не удалось получить подтверждение завершения потока"); break;
            case FailureReason::PreparationFailed: reason = QStringLiteral("Не удалось подготовить операцию"); break;
            case FailureReason::PublicationFailed: reason = QStringLiteral("Не удалось применить результат"); break;
            case FailureReason::InvalidProduct: reason = QStringLiteral("Некорректный результат"); break;
            default: break;
            }
        }
        return QStringLiteral("%1. Операция №%2; результат не применён.").arg(reason).arg(operation.id);
    }
    }
    return {};
}

void ProjectViewModel::setErrorMessage(const QString &message)
{
    if (errorMessage_ == message)
        return;
    errorMessage_ = message;
    emit errorMessageChanged();
}

QVariantMap ProjectViewModel::importDiagnostic() const
{
    const auto &failure = controller_.operation().failure;
    if (!failure || !failure->importError) return {};
    const auto &error = *failure->importError;
    QVariantMap fields{{QStringLiteral("code"), codeName(error.code)},
        {QStringLiteral("phase"), phaseName(error.phase)}, {QStringLiteral("operation"), operationName(error.operation)},
        {QStringLiteral("detail"), error.detail}, {QStringLiteral("deviceCode"), error.deviceCode}};
    if (error.offset) fields.insert(QStringLiteral("offset"), QString::number(*error.offset));
    if (error.source) {
        fields.insert(QStringLiteral("line"), QString::number(error.source->line));
        fields.insert(QStringLiteral("column"), QString::number(error.source->column));
        fields.insert(QStringLiteral("token"), error.source->token);
        if (error.source->coordinate) {
            const auto coordinate = *error.source->coordinate;
            fields.insert(QStringLiteral("coordinate"), coordinate == cache::Coordinate::X ? "X"
                : coordinate == cache::Coordinate::Y ? "Y" : "Z");
        }
    }
    return fields;
}

QString ProjectViewModel::diagnosticText() const
{
    const auto fields = importDiagnostic();
    if (fields.isEmpty()) return {};
    QString text = QStringLiteral("Код: %1 · Фаза: %2 · Операция: %3\n%4")
        .arg(fields.value("code").toString(), fields.value("phase").toString(),
             fields.value("operation").toString(), fields.value("detail").toString());
    if (fields.contains("line"))
        text += QStringLiteral("\nСтрока: %1 · Столбец: %2 · Координата: %3 · Токен: %4")
            .arg(fields.value("line").toString(), fields.value("column").toString(),
                 fields.value("coordinate").toString(), fields.value("token").toString());
    if (fields.contains("offset")) text += QStringLiteral("\nСмещение: %1 байт").arg(fields.value("offset").toString());
    return text + QStringLiteral("\nКод файлового устройства Qt: %1").arg(fields.value("deviceCode").toInt());
}

} // namespace cadcontour
