#pragma once

#include "application/project_controller.h"

#include <QSignalBlocker>
#include <utility>

namespace cadcontour::persistence {
class RecoveryTransactionTestAccess {
public:
    using Hooks = RecoveryTransaction::TestHooks;
    static Result<std::unique_ptr<RecoveryTransaction>> prepare(QString root, RecoveryRecord record,
        std::shared_ptr<const RecoveryLease> lease, RecoveryLimits limits, std::function<bool()> cancel,
        std::shared_ptr<Hooks> hooks)
    { return RecoveryTransaction::prepareImpl(std::move(root), std::move(record), std::move(lease), limits,
                                             std::move(cancel), std::move(hooks)); }
};
}
namespace cadcontour {
class RecoveryTestAccess {
public:
    static QString root(const ProjectController &c) { return c.recoveryRoot_; }
    static bool settled(const ProjectController &c)
    { return !c.operation_.active && !c.recoveryClosing_ && c.retiredRecoveries_.empty(); }
    static void timers(ProjectController &c, int interval = 0)
    {
        c.autosaveSoon_.stop(); c.autosaveTimer_.stop();
        if (interval > 0) c.autosaveTimer_.start(interval);
    }
    static auto blockAutosave(ProjectController &c)
    { return std::pair{QSignalBlocker(c.autosaveSoon_), QSignalBlocker(c.autosaveTimer_)}; }
    static void configure(ProjectController &c, std::shared_ptr<persistence::RecoveryTransactionTestAccess::Hooks> hooks)
    {
        c.recoveryPrepare_ = [hooks = std::move(hooks)](auto root, auto record, auto lease, auto limits, auto cancel) {
            return persistence::RecoveryTransactionTestAccess::prepare(std::move(root), std::move(record),
                std::move(lease), limits, std::move(cancel), hooks);
        };
    }
    static std::shared_ptr<const persistence::RecoveryLease> lease(const ProjectController &c)
    { return c.session_->recoveryLease; }
    static void epoch(ProjectController &c) { ++c.contextEpoch_; }
    static void acceptance(ProjectController &c, std::function<void()> hook) { c.prepareOpenHook_ = std::move(hook); }
};
}
