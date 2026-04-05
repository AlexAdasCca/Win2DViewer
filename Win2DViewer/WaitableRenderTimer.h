#pragma once

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

class WaitableRenderTimer
{
public:
    using TickCallback = std::function<bool()>;

    WaitableRenderTimer() = default;
    ~WaitableRenderTimer();

    WaitableRenderTimer(const WaitableRenderTimer&) = delete;
    WaitableRenderTimer& operator=(const WaitableRenderTimer&) = delete;

    bool Start(std::chrono::nanoseconds interval, TickCallback callback, bool useHighResolution = false);
    void Stop();
    bool IsRunning() const noexcept;

private:
    bool ArmTimer_NoLock() const;
    void WorkerLoop();
    void CloseHandles_NoLock();

private:
    mutable std::mutex stateMutex;
    std::atomic_bool running{ false };
    std::chrono::nanoseconds intervalNs{ 0 };
    TickCallback tickCallback;
    wil::unique_handle stopEvent;
    wil::unique_handle rearmEvent;
    wil::unique_handle timerHandle;
    std::thread workerThread;
};
