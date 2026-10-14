#pragma once

#include "ui/project_view_model.h"

#include <QtQmlIntegration/qqmlintegration.h>

namespace cadcontour {

class OperationProbe : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(OperationProbe)
    QML_UNCREATABLE("OperationProbe is supplied by the verification executable")
    Q_PROPERTY(bool canStart READ canStart NOTIFY changed)
public:
    explicit OperationProbe(ProjectController &controller);
    bool canStart() const noexcept;
    Q_INVOKABLE void startKnown();
    Q_INVOKABLE void startUnknown();
    Q_INVOKABLE void startFailure();
signals:
    void changed();
private:
    void start(bool unknown, bool fail);
    ProjectController &controller_;
};

}
