#include "application_shutdown.h"

#include <QEvent>
#include <QTimer>

namespace cadcontour {

ApplicationShutdown::ApplicationShutdown(QGuiApplication &app, ProjectController &controller, QWindow &window)
    : app_(app), controller_(controller), window_(&window)
{
    app_.setQuitOnLastWindowClosed(false);
    app_.installEventFilter(this);
    connect(&window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible && controller_.shutdownState() == ShutdownState::Ready)
            app_.quit();
    });
    connect(&controller_, &ProjectController::shutdownChanged, this, [this] {
        if (controller_.shutdownState() == ShutdownState::Ready)
            QTimer::singleShot(0, this, [this] {
                if (window_ && window_->isVisible())
                    window_->close();
                else
                    app_.quit();
            });
    });
}

bool ApplicationShutdown::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != &app_ || event->type() != QEvent::Quit)
        return false;
    if (controller_.shutdownState() == ShutdownState::Ready && (!window_ || !window_->isVisible()))
        return false;
    // Window closing enters the presentation's draft/dirty workflow first.
    if (window_)
        QTimer::singleShot(0, this, [this] { if (window_) window_->close(); });
    return true; // keep processing GUI events until Ready and actual window closure
}

}
