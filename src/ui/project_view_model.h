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

    Q_INVOKABLE void applyCell(const QString &text);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void newProject();
    Q_INVOKABLE void cancelOperation();
    Q_INVOKABLE bool requestClose();

signals:
    void stateChanged();
    void errorMessageChanged();
    void cellInputSyncRequested();
    void operationChanged();
    void shutdownChanged();

private:
    void handleResult(const CommandResult &result);
    void setErrorMessage(const QString &message);

    ProjectController &controller_;
    QString errorMessage_;
    std::optional<std::uint64_t> synchronizedIdentity_;
    double synchronizedCell_ = 0;
};

} // namespace cadcontour
