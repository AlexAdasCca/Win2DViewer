#include "pch.h"

#include "WaitableRenderTimer.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

WaitableRenderTimer::~WaitableRenderTimer()
{
    Stop();
}

bool WaitableRenderTimer::Start(std::chrono::nanoseconds interval, TickCallback callback, bool useHighResolution)
{
    if (!callback || interval.count() <= 0)
    {
        Stop();
        return false;
    }
    if (!running.load(std::memory_order_acquire))
    {
        Stop();
    }

    std::scoped_lock lock(stateMutex);
    if (running.load(std::memory_order_acquire))
    {
        intervalNs = interval;
        tickCallback = std::move(callback);
        if (rearmEvent)
        {
            (void)::SetEvent(rearmEvent.get());
        }
        return true;
    }

    intervalNs = interval;
    tickCallback = std::move(callback);
    stopEvent.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent)
    {
        return false;
    }
    rearmEvent.reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!rearmEvent)
    {
        CloseHandles_NoLock();
        return false;
    }

    DWORD timerFlags = 0;
    if (useHighResolution)
    {
        timerFlags |= CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
    }

    timerHandle.reset(::CreateWaitableTimerExW(nullptr, nullptr, timerFlags, TIMER_ALL_ACCESS));
    if (!timerHandle && useHighResolution)
    {
        timerHandle.reset(::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
    }
    if (!timerHandle)
    {
        CloseHandles_NoLock();
        return false;
    }

    if (!ArmTimer_NoLock())
    {
        CloseHandles_NoLock();
        return false;
    }

    running.store(true, std::memory_order_release);
    try
    {
        workerThread = std::thread([this]() { WorkerLoop(); });
    }
    catch (...)
    {
        running.store(false, std::memory_order_release);
        CloseHandles_NoLock();
        return false;
    }
    return true;
}

void WaitableRenderTimer::Stop()
{
    HANDLE stopHandleSnapshot = nullptr;
    HANDLE timerHandleSnapshot = nullptr;
    {
        std::scoped_lock lock(stateMutex);
        if (!stopEvent && !rearmEvent && !timerHandle && !workerThread.joinable())
        {
            running.store(false, std::memory_order_release);
            return;
        }

        stopHandleSnapshot = stopEvent.get();
        timerHandleSnapshot = timerHandle.get();
    }

    if (stopHandleSnapshot != nullptr)
    {
        (void)::SetEvent(stopHandleSnapshot);
    }
    {
        std::scoped_lock lock(stateMutex);
        if (rearmEvent)
        {
            (void)::SetEvent(rearmEvent.get());
        }
    }
    if (timerHandleSnapshot != nullptr)
    {
        (void)::CancelWaitableTimer(timerHandleSnapshot);
    }
    if (workerThread.joinable())
    {
        workerThread.join();
    }

    std::scoped_lock lock(stateMutex);
    running.store(false, std::memory_order_release);
    CloseHandles_NoLock();
}

bool WaitableRenderTimer::IsRunning() const noexcept
{
    return running.load(std::memory_order_acquire);
}

bool WaitableRenderTimer::ArmTimer_NoLock() const
{
    if (!timerHandle || intervalNs.count() <= 0)
    {
        return false;
    }

    using TickDuration = std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>;
    LONGLONG ticks100ns = std::chrono::duration_cast<TickDuration>(intervalNs).count();
    if (ticks100ns <= 0)
    {
        ticks100ns = 1;
    }

    LARGE_INTEGER dueTime{};
    dueTime.QuadPart = -ticks100ns;
    return !!::SetWaitableTimerEx(timerHandle.get(), &dueTime, 0, nullptr, nullptr, nullptr, 0);
}

void WaitableRenderTimer::WorkerLoop()
{
    HANDLE waitHandles[3] = { nullptr, nullptr, nullptr };
    {
        std::scoped_lock lock(stateMutex);
        waitHandles[0] = stopEvent.get();
        waitHandles[1] = rearmEvent.get();
        waitHandles[2] = timerHandle.get();
    }

    while (running.load(std::memory_order_acquire))
    {
        const DWORD waitResult = ::WaitForMultipleObjects(3, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
        {
            break;
        }
        if (waitResult == WAIT_OBJECT_0 + 1)
        {
            std::scoped_lock lock(stateMutex);
            if (!ArmTimer_NoLock())
            {
                break;
            }
            continue;
        }
        if (waitResult != WAIT_OBJECT_0 + 2)
        {
            break;
        }

        TickCallback callback;
        {
            std::scoped_lock lock(stateMutex);
            callback = tickCallback;
        }
        if (!callback || !callback())
        {
            break;
        }

        std::scoped_lock lock(stateMutex);
        if (!ArmTimer_NoLock())
        {
            break;
        }
    }

    running.store(false, std::memory_order_release);
}

void WaitableRenderTimer::CloseHandles_NoLock()
{
    timerHandle.reset();
    stopEvent.reset();
    rearmEvent.reset();
    tickCallback = {};
}
