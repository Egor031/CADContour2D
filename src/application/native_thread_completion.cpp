#include "native_thread_completion.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace cadcontour {

NativeThreadCompletion::~NativeThreadCompletion()
{
    if (const auto handle = handle_.load(std::memory_order_acquire))
        CloseHandle(handle);
}

void NativeThreadCompletion::retain(void *handle) noexcept
{
    void *empty = nullptr;
    if (!handle_.compare_exchange_strong(empty, handle, std::memory_order_release,
                                         std::memory_order_acquire))
        CloseHandle(handle); // owner fallback may already have retained this same thread
}

bool NativeThreadCompletion::captureCurrent() noexcept
{
    threadId_.store(GetCurrentThreadId(), std::memory_order_release);
    HANDLE handle = nullptr;
    if (failDuplicate_.load(std::memory_order_acquire)
        || !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                            &handle, SYNCHRONIZE, FALSE, 0)) {
        error_.store(failDuplicate_.load() ? ERROR_ACCESS_DENIED : GetLastError(),
                     std::memory_order_release);
        return false;
    }
    retain(handle);
    return true;
}

bool NativeThreadCompletion::captureFromOwner() noexcept
{
    if (hasHandle())
        return true;
    const auto id = threadId_.load(std::memory_order_acquire);
    if (!id)
        return false;
    const auto handle = failOpen_.load(std::memory_order_acquire) ? nullptr
                        : OpenThread(SYNCHRONIZE, FALSE, id);
    if (!handle) {
        error_.store(failOpen_.load() ? ERROR_ACCESS_DENIED : GetLastError(),
                     std::memory_order_release);
        return false;
    }
    // Worker cannot leave its decision handshake without a retained handle, so id
    // still denotes the original live thread and cannot have been recycled here.
    retain(handle);
    return true;
}

bool NativeThreadCompletion::finished() noexcept
{
    const auto handle = handle_.load(std::memory_order_acquire);
    if (!handle)
        return false;
    const auto result = failWait_.load(std::memory_order_acquire) ? WAIT_FAILED
                                                               : WaitForSingleObject(handle, 0);
    if (result == WAIT_FAILED)
        error_.store(failWait_.load() ? ERROR_INVALID_HANDLE : GetLastError(), std::memory_order_release);
    return result == WAIT_OBJECT_0; // neither TIMEOUT nor FAILED proves completion
}

bool NativeThreadCompletion::wait() noexcept
{
    const auto handle = handle_.load(std::memory_order_acquire);
    if (!handle)
        return false;
    const auto result = WaitForSingleObject(handle, INFINITE);
    if (result == WAIT_FAILED)
        error_.store(GetLastError(), std::memory_order_release);
    return result == WAIT_OBJECT_0;
}

} // namespace cadcontour
