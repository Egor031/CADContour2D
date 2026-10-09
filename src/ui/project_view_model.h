#pragma once

#include "application/project_controller.h"

#include <QObject>
#include <QString>
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

public:
    explicit ProjectViewModel(ProjectController &controller, QObject *parent = nullptr);

    bool hasProject() const noexcept;
    double cell() const noexcept;
    QString cellText() const;
    QString runtimeIdentity() const;
    QString inputRevision() const;
    QString densityMapStatusText() const;
    bool canUndo() const noexcept { return controller_.canUndo(); }
    bool canRedo() const noexcept { return controller_.canRedo(); }
    QString errorMessage() const { return errorMessage_; }

    Q_INVOKABLE void applyCell(const QString &text);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void newProject();

signals:
    void stateChanged();
    void errorMessageChanged();
    void cellInputSyncRequested();

private:
    void handleResult(const CommandResult &result);
    void setErrorMessage(const QString &message);

    ProjectController &controller_;
    QString errorMessage_;
};

} // namespace cadcontour
