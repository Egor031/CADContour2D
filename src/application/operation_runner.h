#pragma once

#include "operation_types.h"
#include "native_thread_completion.h"

#include <QObject>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace cadcontour {

namespace detail {
struct OperationContext {
    std::atomic<bool> cancellation{false};
    std::atomic<bool> workerEntered{false};
    NativeThreadCompletion nativeCompletion;
    std::mutex mutex;
    std::condition_variable decisionChanged;
    OperationProgress progress;
    bool progressDirty = false;
    bool ready = false;
    bool leased = false;
    bool waitingForDecision = false;
    bool cleanupWarning = false;
    PublicationDecision decision = PublicationDecision::Pending;
    WorkerOutcome outcome;
};
} // namespace detail

class WorkerControl {
public:
    bool cancelled() const noexcept;
    void report(OperationProgress progress) const;
private:
    friend class OperationRunner;
    explicit WorkerControl(std::shared_ptr<detail::OperationContext> context)
        : context_(std::move(context)) {}
    std::shared_ptr<detail::OperationContext> context_;
};

using OperationWork = std::function<WorkerOutcome(const OperationSnapshot &, const WorkerControl &)>;

// Keeps a borrowed ready product alive even if shutdown supplies Discard during preparation.
// No mutex is held while the controller validates or commits.
class ReadyProductLease {
public:
    ReadyProductLease() = default;
    ReadyProductLease(ReadyProductLease &&other) noexcept;
    ReadyProductLease &operator=(ReadyProductLease &&) = delete;
    ~ReadyProductLease();
    explicit operator bool() const noexcept { return bool(context_); }
    const WorkerOutcome &outcome() const noexcept { return context_->outcome; }
    std::unique_ptr<VerificationProduct> takeProduct() noexcept;
private:
    friend class OperationRunner;
    explicit ReadyProductLease(std::shared_ptr<detail::OperationContext> context)
        : context_(std::move(context)) {}
    std::shared_ptr<detail::OperationContext> context_;
};

class OperationRunner final : public QObject {
    Q_OBJECT
public:
    explicit OperationRunner(QObject *parent = nullptr);
    ~OperationRunner() override;
    bool busy() const noexcept { return bool(context_); }
    std::optional<OperationFailure> start(OperationId id, OperationSnapshot snapshot, OperationWork work);
    void cancel() noexcept;
    void shutdown() noexcept;
    void drain() noexcept;
    ReadyProductLease leaseReady(OperationId id);
    bool decide(OperationId id, PublicationDecision decision) noexcept;
    void poll();
signals:
    void entered(cadcontour::OperationId id);
    void progressAvailable(cadcontour::OperationId id, cadcontour::OperationProgress progress);
    void ready(cadcontour::OperationId id);
    void observationFailed(cadcontour::OperationId id, quint32 systemCode);
    void released(cadcontour::OperationId id, cadcontour::WorkerStatus status,
                  cadcontour::OperationFailure failure, bool cleanupWarning);
private:
    friend class OperationRunnerTestAccess;
    // Internal injection seams only; unavailable to production QML/user commands.
    struct TestHooks {
        std::function<std::unique_ptr<QThread>(std::function<void()>)> factory;
        std::function<void()> beforeDecisionWait;
        std::function<void(PublicationDecision)> afterDecision;
        bool skipStart = false;
        bool failDuplicate = false;
        bool failOpen = false;
        bool failNativeWait = false;
    };
    static void execute(const std::shared_ptr<detail::OperationContext> &context,
                        const OperationSnapshot &snapshot, OperationWork &work,
                        const std::shared_ptr<TestHooks> &hooks) noexcept;
    std::shared_ptr<TestHooks> nextHooks_;
    std::shared_ptr<detail::OperationContext> context_;
    std::unique_ptr<QThread> thread_;
    QTimer timer_;
    OperationId id_ = 0;
    bool enteredReported_ = false;
    bool polling_ = false;
    std::uint32_t reportedObservationError_ = 0;
};

} // namespace cadcontour
