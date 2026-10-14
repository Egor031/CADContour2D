#pragma once

#include "application/project_controller.h"

#include <QGuiApplication>
#include <QPointer>
#include <QWindow>

namespace cadcontour {

// Routes posted application Quit events through the same window/controller drain.
class ApplicationShutdown final : public QObject {
public:
    ApplicationShutdown(QGuiApplication &app, ProjectController &controller, QWindow &window);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QGuiApplication &app_;
    ProjectController &controller_;
    QPointer<QWindow> window_;
};

}
