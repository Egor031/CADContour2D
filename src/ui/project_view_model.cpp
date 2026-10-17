#include "project_view_model.h"

#include <QLocale>
#include <QRegularExpression>

namespace cadcontour {

ProjectViewModel::ProjectViewModel(ProjectController &controller, QObject *parent)
    : QObject(parent), controller_(controller)
{
    if (const auto *state = controller_.project()) {
        synchronizedIdentity_ = state->runtimeIdentity();
        synchronizedCell_ = state->cell();
    }
    connect(&controller_, &ProjectController::stateChanged, this, [this] {
        const auto *state = controller_.project();
        const auto identity = state ? std::optional(state->runtimeIdentity()) : std::nullopt;
        const double value = state ? state->cell() : 0;
        const bool sync = identity != synchronizedIdentity_ || value != synchronizedCell_;
        synchronizedIdentity_ = identity;
        synchronizedCell_ = value;
        if (sync)
            setErrorMessage({});
        emit stateChanged();
        if (sync)
            emit cellInputSyncRequested();
        emit operationChanged(); // visibility/text also depend on the current project identity
    });
    connect(&controller_, &ProjectController::operationChanged, this, &ProjectViewModel::operationChanged);
    connect(&controller_, &ProjectController::shutdownChanged, this, &ProjectViewModel::shutdownChanged);
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
    // Task 01 has no constructed datasets; no synthetic publication enters project state.
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
    const auto result = controller_.setCell(value);
    handleResult(result);
    if (result.outcome == CommandOutcome::NoChange) {
        setErrorMessage({});
        emit cellInputSyncRequested(); // only successful Apply canonicalizes a no-op draft
    }
}

void ProjectViewModel::undo() { handleResult(controller_.undo()); }
void ProjectViewModel::redo() { handleResult(controller_.redo()); }
void ProjectViewModel::newProject() { handleResult(controller_.newProject()); }

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
            setErrorMessage(QStringLiteral("Новый проект создаётся после импорта ASC/XYZ. Выбор файла пока недоступен."));
        else if (failure.detail == ErrorDetail::ShuttingDown)
            setErrorMessage(QStringLiteral("Приложение завершает работу."));
        else if (failure.detail == ErrorDetail::WrongThread)
            setErrorMessage(QStringLiteral("Команда вызвана из недопустимого потока."));
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

void ProjectViewModel::cancelOperation() { controller_.cancelOperation(); }
bool ProjectViewModel::requestClose()
{
    controller_.requestShutdown();
    return shutdownReady();
}

bool ProjectViewModel::operationVisible() const noexcept
{
    const auto &operation = controller_.operation();
    return operation.active || (hasProject() && operation.id != 0
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
    return controller_.operation().progress.total.has_value();
}
double ProjectViewModel::progressValue() const noexcept
{
    const auto &progress = controller_.operation().progress;
    return progress.total ? double(progress.completed) / double(*progress.total) : 0;
}
QString ProjectViewModel::phaseText() const
{
    switch (controller_.operation().progress.phase) {
    case OperationPhase::Preparing: return QStringLiteral("Подготовка");
    case OperationPhase::Computing: return QStringLiteral("Вычисление");
    case OperationPhase::Finalizing: return QStringLiteral("Проверка результата");
    case OperationPhase::Cleanup: return QStringLiteral("Освобождение ресурсов");
    }
    return {};
}
QString ProjectViewModel::progressText() const
{
    const auto &progress = controller_.operation().progress;
    if (!progress.total)
        return QStringLiteral("Обработано: %1 · Общий объём неизвестен").arg(progress.completed);
    return QStringLiteral("%1 / %2 · %3 %").arg(progress.completed).arg(*progress.total)
        .arg(100 * progressValue(), 0, 'f', 1);
}
QString ProjectViewModel::operationText() const
{
    const auto &operation = controller_.operation();
    if (closing())
        return QStringLiteral("Завершение приложения: ожидание остановки операции…");
    if (operation.active && (!hasProject()
        || operation.stamp.projectIdentity != controller_.project()->runtimeIdentity()))
        return QStringLiteral("Завершение операции предыдущего проекта…");
    switch (operation.status) {
    case OperationStatus::Idle: return {};
    case OperationStatus::Starting: return QStringLiteral("Запуск фоновой операции…");
    case OperationStatus::Running:
        return operation.publicationRevoked ? QStringLiteral("Входы изменились: ожидание завершения…")
                                            : QStringLiteral("Фоновая операция");
    case OperationStatus::Cancelling: return QStringLiteral("Остановка операции…");
    case OperationStatus::Finishing: return QStringLiteral("Завершение операции…");
    case OperationStatus::Succeeded: return QStringLiteral("Операция завершена.");
    case OperationStatus::Cancelled: return QStringLiteral("Операция отменена. Результат не применён.");
    case OperationStatus::Discarded: return QStringLiteral("Входные данные изменились. Результат не применён.");
    case OperationStatus::Failed: {
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

} // namespace cadcontour
