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
    OperationId id = 0;
    QUuid zRequestId;
    bool zConsumerAvailable = false;
    bool zRequested = false;
    bool zDirty = false;
    bool waitingForZ = false;
    std::optional<NonZeroZRequest> zRequest;
    std::optional<io::ZDecision> zDecision;
    OperationProgress progress;
    bool progressDirty = false;
    bool ready = false;
    bool leased = false;
    bool waitingForDecision = false;
    bool cleanupWarning = false;
    PublicationDecision decision = PublicationDecision::Pending;
    ReadyOutcome outcome;
    std::optional<persistence::SaveCommitResult> saveReceipt;
};
} // namespace detail

class WorkerControl {
public:
    bool cancelled() const noexcept;
    void report(OperationProgress progress) const;
    bool nonZeroZConfirmationAvailable() const;
    io::ZDecision confirmNonZeroZ(io::NonZeroZ z) const;
private:
    friend class OperationRunner;
    explicit WorkerControl(std::shared_ptr<detail::OperationContext> context,
                           std::function<void()> beforeRequest = {},
                           std::function<void()> beforeWait = {})
        : context_(std::move(context)), beforeZRequest_(std::move(beforeRequest)),
          beforeZWait_(std::move(beforeWait)) {}
    std::shared_ptr<detail::OperationContext> context_;
    std::function<void()> beforeZRequest_;
    std::function<void()> beforeZWait_;
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
    const ReadyOutcome &outcome() const noexcept { return context_->outcome; }
    std::unique_ptr<VerificationProduct> takeProduct() noexcept;
    // The noexcept owner exchange and irrevocable Accept share the decision mutex.
    bool accept(void *candidate, void (*commit)(void *) noexcept) noexcept;
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
    std::optional<NonZeroZRequest> pendingNonZeroZ() const;
    bool answerNonZeroZ(const NonZeroZRequest &request, io::ZDecision decision) noexcept;
    void poll();
signals:
    void entered(cadcontour::OperationId id);
    void progressAvailable(cadcontour::OperationId id, cadcontour::OperationProgress progress);
    void ready(cadcontour::OperationId id);
    void nonZeroZConfirmationChanged(cadcontour::OperationId id);
    void observationFailed(cadcontour::OperationId id, quint32 systemCode);
    void saveFinished(cadcontour::OperationId id,
                      std::shared_ptr<const cadcontour::persistence::SavedProject> prepared,
                      cadcontour::persistence::SaveCommitResult receipt);
    void released(cadcontour::OperationId id, cadcontour::WorkerStatus status,
                  cadcontour::OperationFailure failure, bool cleanupWarning);
private:
    friend class OperationRunnerTestAccess;
    // Internal injection seams only; unavailable to production QML/user commands.
    struct TestHooks {
        std::function<std::unique_ptr<QThread>(std::function<void()>)> factory;
        std::function<void()> beforeDecisionWait;
        std::function<void()> beforeZRequest;
        std::function<void()> beforeZWait;
        std::function<void(PublicationDecision)> afterDecision;
        std::function<void(PublicationDecision)> beforeOwnershipFinalization;
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
