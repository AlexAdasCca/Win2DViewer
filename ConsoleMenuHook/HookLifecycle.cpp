#include "pch.h"

#include "HookLifecycle.h"

#include "ConsoleWindowIntegration.h"
#include "HookConfig.h"
#include "HookState.h"
#include "MessageHookBridge.h"

namespace ConsoleMenuHook
{
    namespace
    {
        void StopWindowDiscoveryThreadIfRunning()
        {
            auto& runtimeState = GetRuntimeState();
            if (runtimeState.DiscoveryStopEvent != nullptr)
            {
                (void)::SetEvent(runtimeState.DiscoveryStopEvent);
            }
            if (runtimeState.DiscoveryThread != nullptr)
            {
                (void)::WaitForSingleObject(runtimeState.DiscoveryThread, 2000);
                ::CloseHandle(runtimeState.DiscoveryThread);
                runtimeState.DiscoveryThread = nullptr;
            }
            if (runtimeState.DiscoveryStopEvent != nullptr)
            {
                ::CloseHandle(runtimeState.DiscoveryStopEvent);
                runtimeState.DiscoveryStopEvent = nullptr;
            }
        }
    } // namespace

    DWORD WINAPI InitializeConsoleMenuThread(LPVOID)
    {
        auto& runtimeState = GetRuntimeState();
#if WIN2DVIEWER_CONSOLEMENU_ENABLE_MESSAGE_TIMING_HOOKS
        if (!InstallMessageTimingHooks())
        {
            LogLine(L"[ConsoleMenuHook] Message-timing hook installation failed.");
            return ERROR_PROC_NOT_FOUND;
        }
#else
        LogLine(L"[ConsoleMenuHook] Message-timing hook path disabled by macro.");
#endif

#if WIN2DVIEWER_CONSOLEMENU_ENABLE_MESSAGE_QUIT_HOOKS
        if (!InstallMessageQuitHooks())
        {
            LogLine(L"[ConsoleMenuHook] Message-quit hook installation failed.");
#if WIN2DVIEWER_CONSOLEMENU_ENABLE_MESSAGE_TIMING_HOOKS
            RemoveMessageTimingHooks();
#endif
            return ERROR_PROC_NOT_FOUND;
        }
#else
        LogLine(L"[ConsoleMenuHook] Message-quit hook path disabled by macro.");
#endif

#if WIN2DVIEWER_CONSOLEMENU_ENABLE_WINDOW_ENUM_FALLBACK
        runtimeState.DiscoveryStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (runtimeState.DiscoveryStopEvent != nullptr)
        {
            runtimeState.DiscoveryThread = ::CreateThread(nullptr, 0, &WindowDiscoveryThreadProc, nullptr, 0, nullptr);
        }
#else
        LogLine(L"[ConsoleMenuHook] Window-enumeration fallback path disabled by macro.");
#endif

        DWORD result = ERROR_TIMEOUT;
        for (int attempt = 0; attempt < 400; ++attempt)
        {
            HWND windowHandle = runtimeState.ConsoleWindow.load();
            if (windowHandle != nullptr && ::IsWindow(windowHandle) && runtimeState.ConsoleMenuInstalled.load() &&
                runtimeState.ConsoleWindowSubclassed.load())
            {
                result = 0;
                break;
            }

            ::Sleep(25);
        }

#if WIN2DVIEWER_CONSOLEMENU_ENABLE_WINDOW_ENUM_FALLBACK
        StopWindowDiscoveryThreadIfRunning();
#endif

#if WIN2DVIEWER_CONSOLEMENU_ENABLE_MESSAGE_TIMING_HOOKS
        RemoveMessageTimingHooks();
#endif
        // Keep quit hooks alive for the entire console-host lifetime.
        // They are removed in DLL_PROCESS_DETACH.
        if (result == ERROR_TIMEOUT)
        {
            LogLine(L"[ConsoleMenuHook] Timed out while waiting for captured console window.");
        }
        return result;
    }

    void CleanupConsoleMenuHook()
    {
        auto& runtimeState = GetRuntimeState();
        RestoreOriginalWindowProcedureIfNeeded();

#if WIN2DVIEWER_CONSOLEMENU_ENABLE_MESSAGE_TIMING_HOOKS
        RemoveMessageTimingHooks();
#endif
#if WIN2DVIEWER_CONSOLEMENU_ENABLE_MESSAGE_QUIT_HOOKS
        RemoveMessageQuitHooks();
#endif
#if WIN2DVIEWER_CONSOLEMENU_ENABLE_WINDOW_ENUM_FALLBACK
        StopWindowDiscoveryThreadIfRunning();
#endif

        if (runtimeState.InitThread != nullptr)
        {
            ::CloseHandle(runtimeState.InitThread);
            runtimeState.InitThread = nullptr;
        }
    }
} // namespace ConsoleMenuHook
