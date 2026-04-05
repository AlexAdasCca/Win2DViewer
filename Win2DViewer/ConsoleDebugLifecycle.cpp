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
        wil::unique_handle gConsoleCloseNotifyEvent;
        wil::unique_handle gConsoleCtrlFallbackEvent;
        wil::unique_handle gConsoleMonitorStopEvent;
        wil::unique_handle gConsoleMonitorThread;
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

            DiagnosticConsole::LineBuilder line;
            line << L"[ConsoleDebug] close-signal source=" << source << L", detaching console.";
            DiagnosticConsole::WriteLine(line.str());
            StopConsoleCloseMonitor();

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
            ConsoleMenu::SetInjectionDiagnosticsEnabled(false);
            ConsoleMenu::ResetConsoleHookState();
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
                    DiagnosticConsole::WriteLine(L"[ConsoleDebug] Ctrl+C/Ctrl+Break is blocked in debug-console mode. "
                                                 L"Use interactive window close instead.");
                    return TRUE;

                case CTRL_CLOSE_EVENT:
                case CTRL_LOGOFF_EVENT:
                case CTRL_SHUTDOWN_EVENT:
                    // Close/logoff/shutdown notifications are best-effort cleanup signals.
                    // Returning TRUE requests handling, but Windows can still terminate the
                    // process afterward. We therefore trigger fallback cleanup immediately.
                    if (gConsoleCtrlFallbackEvent)
                    {
                        (void)::SetEvent(gConsoleCtrlFallbackEvent.get());
                    }
                    return TRUE;

                default:
                    return FALSE;
            }
        }

        DWORD WINAPI ConsoleCloseMonitorThreadProc(LPVOID)
        {
            HANDLE waits[] = { gConsoleMonitorStopEvent.get(),
                               gConsoleCloseNotifyEvent.get(),
                               gConsoleCtrlFallbackEvent.get() };
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
                    return 0;
                }
                if (waitResult == WAIT_OBJECT_0 + 2)
                {
                    DetachDebugConsoleAfterCloseSignal(L"ConsoleCtrlHandlerFallback");
                    return 0;
                }
                return 1;
            }
        }

        bool StartConsoleCloseMonitor()
        {
            if (gConsoleMonitorThread)
            {
                return true;
            }

            const std::wstring notifyEventName =
                ConsoleHookIpc::BuildConsoleCloseNotifyEventName(::GetCurrentProcessId());
            auto closeNotifyEvent = wil::unique_handle(::CreateEventW(nullptr, FALSE, FALSE, notifyEventName.c_str()));
            auto ctrlFallbackEvent = wil::unique_handle(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
            auto monitorStopEvent = wil::unique_handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!closeNotifyEvent || !ctrlFallbackEvent || !monitorStopEvent)
            {
                StopConsoleCloseMonitor();
                return false;
            }

            if (!gConsoleCtrlHandlerInstalled)
            {
                gConsoleCtrlHandlerInstalled = !!::SetConsoleCtrlHandler(&DebugConsoleCtrlHandler, TRUE);
            }

            auto monitorThread =
                wil::unique_handle(::CreateThread(nullptr, 0, &ConsoleCloseMonitorThreadProc, nullptr, 0, nullptr));
            if (!monitorThread)
            {
                StopConsoleCloseMonitor();
                return false;
            }

            gConsoleCloseNotifyEvent = std::move(closeNotifyEvent);
            gConsoleCtrlFallbackEvent = std::move(ctrlFallbackEvent);
            gConsoleMonitorStopEvent = std::move(monitorStopEvent);
            gConsoleMonitorThread = std::move(monitorThread);
            return true;
        }

        void StopConsoleCloseMonitor()
        {
            if (gConsoleCtrlHandlerInstalled)
            {
                (void)::SetConsoleCtrlHandler(&DebugConsoleCtrlHandler, FALSE);
                gConsoleCtrlHandlerInstalled = false;
            }

            if (gConsoleMonitorStopEvent)
            {
                (void)::SetEvent(gConsoleMonitorStopEvent.get());
            }

            if (gConsoleMonitorThread)
            {
                if (::GetCurrentThreadId() != ::GetThreadId(gConsoleMonitorThread.get()))
                {
                    (void)::WaitForSingleObject(gConsoleMonitorThread.get(), 2000);
                }
            }

            gConsoleMonitorThread.reset();
            gConsoleMonitorStopEvent.reset();
            gConsoleCloseNotifyEvent.reset();
            gConsoleCtrlFallbackEvent.reset();
        }
    } // namespace

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
        DiagnosticConsole::ConfigureUnicodeConsole();
        gConsoleAllocated = true;
        gConsoleOwnedByApp = true;
        ConsoleMenu::SetInjectionDiagnosticsEnabled(true);
        DiagnosticConsole::WriteLine(L"[ConsoleDebug] Console attached.");
        (void)StartConsoleCloseMonitor();
        ConsoleMenu::BeginConsoleHookInjectionAsync();
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
        ConsoleMenu::SetInjectionDiagnosticsEnabled(false);
        ConsoleMenu::ResetConsoleHookState();
    }

    void DebugPrintLine(std::wstring const& line)
    {
        if (!gConsoleAllocated)
        {
            return;
        }

        DiagnosticConsole::WriteLine(line);
    }
} // namespace ConsoleDebugLifecycle
