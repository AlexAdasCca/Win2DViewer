#include "pch.h"

#include "ConsoleDebugLifecycle.h"

#include "ConsoleDebugMessages.h"
#include "ConsoleHookIpc.h"
#include "ConsoleMenuInjection.h"
#include "ConsoleTextWriter.h"

#include <cstdio>

namespace ConsoleDebugLifecycle
{
    namespace
    {
        bool gConsoleAllocated = false;
        bool gConsoleOwnedByApp = false;
        bool gConsoleCtrlHandlerInstalled = false;
        HANDLE gConsoleCloseNotifyEvent = nullptr;
        HANDLE gConsoleCtrlFallbackEvent = nullptr;
        HANDLE gConsoleMonitorStopEvent = nullptr;
        HANDLE gConsoleMonitorThread = nullptr;
        HWND gConsoleStateSyncTargetWindow = nullptr;
        std::atomic_bool gConsoleDetachInProgress = false;

        void StopConsoleCloseMonitor();

        void DetachDebugConsoleAfterCloseSignal(std::wstring const& source)
        {
            if (!gConsoleAllocated)
            {
                return;
            }

            bool expected = false;
            if (!gConsoleDetachInProgress.compare_exchange_strong(expected, true))
            {
                return;
            }

            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleDebug] close-signal source=" << source << L", detaching console.";
            diagnosticconsole::WriteLine(line.str());

            if (gConsoleOwnedByApp)
            {
                (void)::FreeConsole();
            }

            FILE* stream = nullptr;
            freopen_s(&stream, "NUL:", "w", stdout);
            freopen_s(&stream, "NUL:", "w", stderr);
            freopen_s(&stream, "NUL:", "r", stdin);

            gConsoleAllocated = false;
            gConsoleOwnedByApp = false;
            consolemenu::SetInjectionDiagnosticsEnabled(false);
            consolemenu::ResetConsoleHookState();
            if (gConsoleStateSyncTargetWindow != nullptr && ::IsWindow(gConsoleStateSyncTargetWindow))
            {
                ::PostMessageW(gConsoleStateSyncTargetWindow, kConsoleDebugStateSyncMsg, FALSE, 0);
            }
            gConsoleDetachInProgress.store(false);
        }

        BOOL WINAPI DebugConsoleCtrlHandler(DWORD ctrlType)
        {
            switch (ctrlType)
            {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT:
                // Returning TRUE here consumes the keyboard control event for this process,
                // so the default console handler does not terminate the process.
                //
                // References:
                // https://learn.microsoft.com/windows/console/handlerroutine
                // https://learn.microsoft.com/windows/console/setconsolectrlhandler
                diagnosticconsole::WriteLine(
                    L"[ConsoleDebug] Ctrl+C/Ctrl+Break is blocked in debug-console mode. "
                    L"Use interactive window close instead.");
                return TRUE;

            case CTRL_CLOSE_EVENT:
            case CTRL_LOGOFF_EVENT:
            case CTRL_SHUTDOWN_EVENT:
                // Close/logoff/shutdown notifications are best-effort cleanup signals.
                // Returning TRUE requests handling, but Windows can still terminate the
                // process afterward. We therefore trigger fallback cleanup immediately.
                if (gConsoleCtrlFallbackEvent != nullptr)
                {
                    (void)::SetEvent(gConsoleCtrlFallbackEvent);
                }
                return TRUE;

            default:
                return FALSE;
            }
        }

        DWORD WINAPI ConsoleCloseMonitorThreadProc(LPVOID)
        {
            HANDLE waits[] = { gConsoleMonitorStopEvent, gConsoleCloseNotifyEvent, gConsoleCtrlFallbackEvent };
            for (;;)
            {
                const DWORD waitResult = ::WaitForMultipleObjects(_countof(waits), waits, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0)
                {
                    return 0;
                }
                if (waitResult == WAIT_OBJECT_0 + 1)
                {
                    DetachDebugConsoleAfterCloseSignal(L"ConhostSubclassNotify");
                    continue;
                }
                if (waitResult == WAIT_OBJECT_0 + 2)
                {
                    DetachDebugConsoleAfterCloseSignal(L"ConsoleCtrlHandlerFallback");
                    continue;
                }
                return 1;
            }
        }

        bool StartConsoleCloseMonitor()
        {
            if (gConsoleMonitorThread != nullptr)
            {
                return true;
            }

            const std::wstring notifyEventName = consolehookipc::BuildConsoleCloseNotifyEventName(::GetCurrentProcessId());
            gConsoleCloseNotifyEvent = ::CreateEventW(nullptr, FALSE, FALSE, notifyEventName.c_str());
            gConsoleCtrlFallbackEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            gConsoleMonitorStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (gConsoleCloseNotifyEvent == nullptr || gConsoleCtrlFallbackEvent == nullptr || gConsoleMonitorStopEvent == nullptr)
            {
                StopConsoleCloseMonitor();
                return false;
            }

            if (!gConsoleCtrlHandlerInstalled)
            {
                gConsoleCtrlHandlerInstalled = !!::SetConsoleCtrlHandler(&DebugConsoleCtrlHandler, TRUE);
            }

            gConsoleMonitorThread = ::CreateThread(nullptr, 0, &ConsoleCloseMonitorThreadProc, nullptr, 0, nullptr);
            if (gConsoleMonitorThread == nullptr)
            {
                StopConsoleCloseMonitor();
                return false;
            }

            return true;
        }

        void StopConsoleCloseMonitor()
        {
            if (gConsoleCtrlHandlerInstalled)
            {
                (void)::SetConsoleCtrlHandler(&DebugConsoleCtrlHandler, FALSE);
                gConsoleCtrlHandlerInstalled = false;
            }

            if (gConsoleMonitorStopEvent != nullptr)
            {
                (void)::SetEvent(gConsoleMonitorStopEvent);
            }

            if (gConsoleMonitorThread != nullptr)
            {
                if (::GetCurrentThreadId() != ::GetThreadId(gConsoleMonitorThread))
                {
                    (void)::WaitForSingleObject(gConsoleMonitorThread, 2000);
                }
                ::CloseHandle(gConsoleMonitorThread);
                gConsoleMonitorThread = nullptr;
            }

            if (gConsoleMonitorStopEvent != nullptr)
            {
                ::CloseHandle(gConsoleMonitorStopEvent);
                gConsoleMonitorStopEvent = nullptr;
            }
            if (gConsoleCloseNotifyEvent != nullptr)
            {
                ::CloseHandle(gConsoleCloseNotifyEvent);
                gConsoleCloseNotifyEvent = nullptr;
            }
            if (gConsoleCtrlFallbackEvent != nullptr)
            {
                ::CloseHandle(gConsoleCtrlFallbackEvent);
                gConsoleCtrlFallbackEvent = nullptr;
            }
        }
    }

    void SetStateSyncTargetWindow(HWND targetWindow)
    {
        gConsoleStateSyncTargetWindow = targetWindow;
    }

    void EnsureDebugConsole()
    {
        if (gConsoleAllocated)
        {
            return;
        }

        if (!::AllocConsole())
        {
            return;
        }

        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
        diagnosticconsole::ConfigureUnicodeConsole();
        gConsoleAllocated = true;
        gConsoleOwnedByApp = true;
        consolemenu::SetInjectionDiagnosticsEnabled(true);
        diagnosticconsole::WriteLine(L"[ConsoleDebug] Console attached.");
        (void)StartConsoleCloseMonitor();
        consolemenu::BeginConsoleHookInjectionAsync();
    }

    void ReleaseDebugConsole()
    {
        StopConsoleCloseMonitor();

        if (gConsoleAllocated)
        {
            fflush(stdout);
            fflush(stderr);

            FILE* stream = nullptr;
            freopen_s(&stream, "NUL:", "w", stdout);
            freopen_s(&stream, "NUL:", "w", stderr);
            freopen_s(&stream, "NUL:", "r", stdin);

            if (gConsoleOwnedByApp)
            {
                (void)::FreeConsole();
            }
        }

        gConsoleAllocated = false;
        gConsoleOwnedByApp = false;
        consolemenu::SetInjectionDiagnosticsEnabled(false);
        consolemenu::ResetConsoleHookState();
    }

    void DebugPrintLine(std::wstring const& line)
    {
        if (!gConsoleAllocated)
        {
            return;
        }

        diagnosticconsole::WriteLine(line);
    }
}
