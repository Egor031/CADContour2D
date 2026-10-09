#include "project_view_model.h"

#include <QLocale>
#include <QRegularExpression>

namespace cadcontour {

ProjectViewModel::ProjectViewModel(ProjectController &controller, QObject *parent)
    : QObject(parent), controller_(controller)
{
    connect(&controller_, &ProjectController::stateChanged, this, [this] {
        setErrorMessage({});
        emit stateChanged();
        emit cellInputSyncRequested();
    });
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
    handleResult(controller_.setCell(value));
}

void ProjectViewModel::undo() { handleResult(controller_.undo()); }
void ProjectViewModel::redo() { handleResult(controller_.redo()); }
void ProjectViewModel::newProject() { handleResult(controller_.newProject()); }

void ProjectViewModel::handleResult(const CommandResult &result)
{
    if (result.outcome != CommandOutcome::Rejected) {
        setErrorMessage({});
        if (result.outcome == CommandOutcome::NoChange)
            emit cellInputSyncRequested();
        return;
    }
    const auto &failure = *result.failure;
    switch (failure.category) {
    case CommandError::InvalidValue:
        setErrorMessage(QStringLiteral("Размер ячейки должен быть конечным числом строго больше нуля."));
        break;
    case CommandError::NoProject:
        setErrorMessage(QStringLiteral("Нет открытого проекта. Создайте новый проект."));
        break;
    case CommandError::UnsafeState:
        setErrorMessage(failure.detail == ErrorDetail::ReentrantCommand
            ? QStringLiteral("Предыдущая команда ещё не завершена.")
            : QStringLiteral("Невозможно безопасно изменить состояние: исчерпан внутренний счётчик."));
        break;
    }
}

void ProjectViewModel::setErrorMessage(const QString &message)
{
    if (errorMessage_ == message)
        return;
    errorMessage_ = message;
    emit errorMessageChanged();
}

} // namespace cadcontour
