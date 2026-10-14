#pragma once

#include "operation_test_access.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace cadcontour::test {

class Event {
public:
    void set() { { std::lock_guard lock(mutex_); set_ = true; } changed_.notify_all(); }
    bool isSet() { std::lock_guard lock(mutex_); return set_; }
    void wait()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(10), [this] { return set_; }))
            throw std::runtime_error("test barrier deadline exceeded");
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

template<class Predicate> bool spinUntil(Predicate predicate)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!predicate()) {
        if (deadline.elapsed() > 10000)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::yieldCurrentThread();
    }
    return true;
}

inline bool finish(ProjectController &controller)
{
    return spinUntil([&] { OperationTestAccess::poll(controller); return !controller.operation().active; });
}

struct ControlledWork {
private:
    struct Data {
        Event entered;
        Event complete;
        QThread *workerThread = nullptr;
        OperationSnapshot snapshot{};
    };
    std::shared_ptr<Data> data_ = std::make_shared<Data>();
public:
    Event &entered = data_->entered;
    Event &complete = data_->complete;
    QThread *&workerThread = data_->workerThread;
    OperationSnapshot &snapshot = data_->snapshot;
    OperationWork function(std::uint64_t value = 42, bool obeyCancellation = false)
    {
        return [data = data_, value, obeyCancellation](const OperationSnapshot &input, const WorkerControl &control) {
            data->workerThread = QThread::currentThread();
            data->snapshot = input;
            control.report({OperationPhase::Computing, 1, std::nullopt});
            data->entered.set();
            data->complete.wait();
            if (obeyCancellation && control.cancelled())
                return WorkerOutcome::cancelled();
            auto product = std::make_unique<VerificationProduct>();
            product->sourceCell = input.cell;
            product->value = value;
            return WorkerOutcome::success(std::move(product));
        };
    }
    ~ControlledWork() { complete.set(); }
};

inline bool waitContext(const std::shared_ptr<detail::OperationContext> &context, bool waiting)
{
    std::unique_lock lock(context->mutex);
    return context->decisionChanged.wait_for(lock, std::chrono::seconds(10), [&] {
        return waiting ? context->waitingForDecision : context->ready;
    });
}

inline WorkerOutcome success(const OperationSnapshot &snapshot, std::uint64_t value = 42)
{
    auto product = std::make_unique<VerificationProduct>();
    product->sourceCell = snapshot.cell;
    product->value = value;
    return WorkerOutcome::success(std::move(product));
}

} // namespace cadcontour::test
