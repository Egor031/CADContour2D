#pragma once

#include <atomic>
#include <cstdint>

namespace cadcontour {

// Windows thread object stays valid independently of Qt's internal handle lifetime.
class NativeThreadCompletion final {
public:
    NativeThreadCompletion() = default;
    ~NativeThreadCompletion();
    NativeThreadCompletion(const NativeThreadCompletion &) = delete;
    NativeThreadCompletion &operator=(const NativeThreadCompletion &) = delete;
    bool captureCurrent() noexcept;
    bool captureFromOwner() noexcept;
    bool hasHandle() const noexcept { return handle_.load(std::memory_order_acquire) != nullptr; }
    bool finished() noexcept;
    bool wait() noexcept;
    std::uint32_t error() const noexcept { return error_.load(std::memory_order_acquire); }
private:
    friend class OperationRunner;
    friend class OperationRunnerTestAccess;
    void retain(void *handle) noexcept;
    std::atomic<void *> handle_{nullptr};
    std::atomic<std::uint32_t> threadId_{0};
    std::atomic<std::uint32_t> error_{0};
    // Fault injection is private and never exposed to production commands/QML.
    std::atomic<bool> failDuplicate_{false}, failOpen_{false}, failWait_{false};
};

} // namespace cadcontour
