#include "pch.h"

#include <detours/detours.h>
#include <winnt.h>
#include <winternl.h>

#include "..\Win2DViewer\ConsoleHookIpc.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace
{
    constexpr UINT_PTR kConsoleMenuSubclassId = 0x43534D48;
    constexpr UINT kConsoleHookIntegrateMsg = WM_APP + 0x2A7;

    using TranslateMessageFn = BOOL(WINAPI*)(const MSG*);
    using DispatchMessageWFn = LRESULT(WINAPI*)(const MSG*);
    using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

    struct ProcessBasicInformationData
    {
        PVOID Reserved1 = nullptr;
        PPEB PebBaseAddress = nullptr;
        PVOID Reserved2[2]{};
        ULONG_PTR UniqueProcessId = 0;
        ULONG_PTR InheritedFromUniqueProcessId = 0;
    };

    std::atomic<HWND> gConsoleWindow{ nullptr };
    bool gConsoleWindowTopMost = false;
    HANDLE gInitThread = nullptr;
    systemmenu::MenuHost gConsoleMenuHost{ L"Conhost.SystemMenu" };
    std::atomic_bool gConsoleMenuInstalled = false;
    std::atomic_bool gConsoleWindowSubclassed = false;
    std::atomic_bool gConsoleCloseNotified = false;
    HANDLE gDiscoveryStopEvent = nullptr;
    HANDLE gDiscoveryThread = nullptr;

    TranslateMessageFn gOriginalTranslateMessage = ::TranslateMessage;
    DispatchMessageWFn gOriginalDispatchMessageW = ::DispatchMessageW;

    void ShowAboutDialog(HWND ownerWindow);
    bool EnsureConsoleWindowIntegratedOnOwnerThread(HWND windowHandle);
    HWND FindConsoleWindowForCurrentProcess();
    void TryIntegrateConsoleWindow(HWND windowHandle, std::wstring const& source);
    LRESULT CALLBACK ConsoleWindowSubclassProc(
        HWND windowHandle,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);


    void LogLine(std::wstring const& line)
    {
        diagnosticconsole::ConfigureUnicodeConsole();
        diagnosticconsole::WriteLine(line);
    }

    DWORD QueryParentProcessId()
    {
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr)
        {
            return 0;
        }

        const auto ntQueryInformationProcess =
            reinterpret_cast<NtQueryInformationProcessFn>(::GetProcAddress(ntdll, "NtQueryInformationProcess"));
        if (ntQueryInformationProcess == nullptr)
        {
            return 0;
        }

        ProcessBasicInformationData info{};
        const NTSTATUS status = ntQueryInformationProcess(
            ::GetCurrentProcess(),
            ProcessBasicInformation,
            &info,
            sizeof(info),
            nullptr);
        if (!NT_SUCCESS(status))
        {
            return 0;
        }

        return static_cast<DWORD>(info.InheritedFromUniqueProcessId);
    }

    void NotifyOwnerConsoleClose(std::wstring const& source)
    {
        bool expected = false;
        if (!gConsoleCloseNotified.compare_exchange_strong(expected, true))
        {
            return;
        }

        const DWORD ownerPid = QueryParentProcessId();
        if (ownerPid == 0)
        {
            LogLine(L"[ConsoleMenuHook] Console close notify failed: parent pid query failed.");
            return;
        }

        const std::wstring eventName = consolehookipc::BuildConsoleCloseNotifyEventName(ownerPid);
        HANDLE notifyEvent = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
        if (notifyEvent == nullptr)
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Console close notify open failed. source="
                 << source
                 << L" ownerPid=" << ownerPid
                 << L" gle=" << ::GetLastError();
            LogLine(line.str());
            return;
        }

        if (!::SetEvent(notifyEvent))
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Console close notify SetEvent failed. source="
                 << source
                 << L" ownerPid=" << ownerPid
                 << L" gle=" << ::GetLastError();
            LogLine(line.str());
            ::CloseHandle(notifyEvent);
            return;
        }

        ::CloseHandle(notifyEvent);
        diagnosticconsole::LineBuilder line;
        line << L"[ConsoleMenuHook] Console close notify sent. source="
             << source
             << L" ownerPid=" << ownerPid;
        LogLine(line.str());
    }

    void CaptureConsoleWindow(HWND windowHandle)
    {
        if (windowHandle == nullptr || !::IsWindow(windowHandle))
        {
            return;
        }

        wchar_t className[64]{};
        if (::GetClassNameW(windowHandle, className, _countof(className)) == 0)
        {
            return;
        }

        if (std::wstring_view(className) != L"ConsoleWindowClass")
        {
            return;
        }

        HWND expected = nullptr;
        if (gConsoleWindow.compare_exchange_strong(expected, windowHandle))
        {
            gConsoleCloseNotified.store(false);
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Captured console window hwnd=0x"
                 << std::hex << reinterpret_cast<ULONG_PTR>(windowHandle);
            LogLine(line.str());
        }
    }

    BOOL CALLBACK FindConsoleWindowEnumProc(HWND windowHandle, LPARAM lParam)
    {
        wchar_t className[64]{};
        if (::GetClassNameW(windowHandle, className, _countof(className)) == 0)
        {
            return TRUE;
        }
        if (std::wstring_view(className) != L"ConsoleWindowClass")
        {
            return TRUE;
        }

        DWORD processId = 0;
        (void)::GetWindowThreadProcessId(windowHandle, &processId);
        if (processId != ::GetCurrentProcessId())
        {
            return TRUE;
        }

        *reinterpret_cast<HWND*>(lParam) = windowHandle;
        return FALSE;
    }

    HWND FindConsoleWindowForCurrentProcess()
    {
        HWND foundWindow = nullptr;
        (void)::EnumWindows(&FindConsoleWindowEnumProc, reinterpret_cast<LPARAM>(&foundWindow));
        return foundWindow;
    }

    bool EnsureConsoleMenuModel()
    {
        if (!gConsoleMenuHost.Items().empty())
        {
            return true;
        }

        systemmenu::MenuItemSpec separator{};
        separator.separator = true;

        systemmenu::MenuItemSpec topMostItem{};
        topMostItem.id = systemmenu::kCommandWindowTopMost;
        topMostItem.text = L"窗口置顶";
        topMostItem.shortcut = systemmenu::ShortcutBinding{};
        topMostItem.shortcut->virtualKey = 'T';
        topMostItem.shortcut->ctrl = true;
        topMostItem.shortcut->alt = true;
        topMostItem.onInvoke = [](HWND ownerWindow)
        {
            gConsoleWindowTopMost = !gConsoleWindowTopMost;
            ::SetWindowPos(
                ownerWindow,
                gConsoleWindowTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        };
        topMostItem.isChecked = []() { return gConsoleWindowTopMost; };
        topMostItem.isEnabled = []() { return true; };

        systemmenu::MenuItemSpec aboutItem{};
        aboutItem.id = systemmenu::kCommandAbout;
        aboutItem.text = L"打开关于";
        aboutItem.shortcut = systemmenu::ShortcutBinding{};
        aboutItem.shortcut->virtualKey = VK_F1;
        aboutItem.shortcut->alt = true;
        aboutItem.onInvoke = [](HWND ownerWindow) { ShowAboutDialog(ownerWindow); };
        aboutItem.isEnabled = []() { return true; };

        std::wstring errorMessage;
        if (!gConsoleMenuHost.AddItem(separator, &errorMessage) ||
            !gConsoleMenuHost.AddItem(std::move(topMostItem), &errorMessage) ||
            !gConsoleMenuHost.AddItem(std::move(aboutItem), &errorMessage))
        {
            LogLine(L"[ConsoleMenuHook] Failed to build menu model: " + errorMessage);
            return false;
        }

        return true;
    }

    void TryIntegrateConsoleWindow(HWND windowHandle, std::wstring const& source)
    {
        if (windowHandle == nullptr || !::IsWindow(windowHandle))
        {
            return;
        }

        CaptureConsoleWindow(windowHandle);
        if (gConsoleWindow.load() != windowHandle || gConsoleWindowSubclassed.load())
        {
            return;
        }

        if (EnsureConsoleWindowIntegratedOnOwnerThread(windowHandle))
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Integrated console window via " << source;
            LogLine(line.str());
        }
    }

    BOOL WINAPI HookedTranslateMessage(const MSG* message)
    {
        if (message != nullptr && message->hwnd != nullptr)
        {
            TryIntegrateConsoleWindow(message->hwnd, L"TranslateMessage");
        }

        return gOriginalTranslateMessage != nullptr ? gOriginalTranslateMessage(message) : FALSE;
    }

    LRESULT WINAPI HookedDispatchMessageW(const MSG* message)
    {
        if (message != nullptr && message->hwnd != nullptr)
        {
            TryIntegrateConsoleWindow(message->hwnd, L"DispatchMessageW");

            if (gConsoleWindow.load() == message->hwnd)
            {
                if (message->message == WM_CLOSE)
                {
                    NotifyOwnerConsoleClose(L"HookedDispatchMessageW/WM_CLOSE");
                    return 0;
                }
                if (message->message == WM_SYSCOMMAND && (message->wParam & 0xFFF0) == SC_CLOSE)
                {
                    NotifyOwnerConsoleClose(L"HookedDispatchMessageW/SC_CLOSE");
                    return 0;
                }
            }
        }

        return gOriginalDispatchMessageW != nullptr ? gOriginalDispatchMessageW(message) : 0;
    }

    bool InstallMessageTimingHooks()
    {
        LONG status = DetourTransactionBegin();
        if (status != NO_ERROR)
        {
            return false;
        }

        status = DetourUpdateThread(::GetCurrentThread());
        if (status != NO_ERROR)
        {
            (void)DetourTransactionAbort();
            return false;
        }

        status = DetourAttach(reinterpret_cast<PVOID*>(&gOriginalTranslateMessage), HookedTranslateMessage);
        if (status == NO_ERROR)
        {
            status = DetourAttach(reinterpret_cast<PVOID*>(&gOriginalDispatchMessageW), HookedDispatchMessageW);
        }

        if (status != NO_ERROR)
        {
            (void)DetourTransactionAbort();
            return false;
        }

        status = DetourTransactionCommit();
        if (status != NO_ERROR)
        {
            return false;
        }

        LogLine(L"[ConsoleMenuHook] Installed TranslateMessage/DispatchMessageW hooks.");
        return true;
    }

    void RemoveMessageTimingHooks()
    {
        LONG status = DetourTransactionBegin();
        if (status != NO_ERROR)
        {
            return;
        }

        status = DetourUpdateThread(::GetCurrentThread());
        if (status != NO_ERROR)
        {
            (void)DetourTransactionAbort();
            return;
        }

        (void)DetourDetach(reinterpret_cast<PVOID*>(&gOriginalDispatchMessageW), HookedDispatchMessageW);
        (void)DetourDetach(reinterpret_cast<PVOID*>(&gOriginalTranslateMessage), HookedTranslateMessage);
        status = DetourTransactionCommit();
        if (status == NO_ERROR)
        {
            LogLine(L"[ConsoleMenuHook] Removed TranslateMessage/DispatchMessageW hooks.");
        }
    }

    void ShowAboutDialog(HWND ownerWindow)
    {
        ::MessageBoxW(
            ownerWindow,
            L"Win2DViewer Debug Console\nInjected into conhost for system menu extension.",
            L"Win2DViewer",
            MB_OK | MB_ICONINFORMATION);
    }

    LRESULT CALLBACK ConsoleWindowSubclassProc(
        HWND windowHandle,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR)
    {
        switch (message)
        {
        case WM_INITMENUPOPUP:
            gConsoleMenuHost.RefreshState(reinterpret_cast<HMENU>(wParam));
            break;

        case WM_SYSCOMMAND:
        {
            const UINT_PTR commandId = (wParam & 0xFFF0);
            if (commandId == SC_CLOSE)
            {
                NotifyOwnerConsoleClose(L"Subclass/SC_CLOSE");
                return 0;
            }
            if (gConsoleMenuHost.HandleCommand(windowHandle, commandId))
            {
                diagnosticconsole::LineBuilder line;
                line << L"[ConsoleMenuHook] Handled subclass WM_SYSCOMMAND id=0x" << std::hex << commandId;
                LogLine(line.str());
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            NotifyOwnerConsoleClose(L"Subclass/WM_CLOSE");
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (gConsoleMenuHost.HandleShortcut(windowHandle, message, wParam))
            {
                LogLine(L"[ConsoleMenuHook] Handled subclass shortcut.");
                return 0;
            }
            break;

        case WM_NCDESTROY:
            NotifyOwnerConsoleClose(L"Subclass/WM_NCDESTROY");
            ::RemoveWindowSubclass(windowHandle, &ConsoleWindowSubclassProc, subclassId);
            gConsoleWindow.store(nullptr);
            gConsoleMenuInstalled.store(false);
            gConsoleWindowSubclassed.store(false);
            gConsoleCloseNotified.store(false);
            break;
        }

        return ::DefSubclassProc(windowHandle, message, wParam, lParam);
    }

    void InstallConsoleWindowMenu(HWND windowHandle)
    {
        if (windowHandle == nullptr)
        {
            LogLine(L"[ConsoleMenuHook] InstallConsoleWindowMenu received a null hwnd.");
            return;
        }

        HMENU systemMenu = ::GetSystemMenu(windowHandle, FALSE);
        if (systemMenu == nullptr)
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] GetSystemMenu failed. gle=" << ::GetLastError();
            LogLine(line.str());
            return;
        }

        if (!EnsureConsoleMenuModel())
        {
            return;
        }

        const bool topMostExists =
            ::GetMenuState(systemMenu, static_cast<UINT>(systemmenu::kCommandWindowTopMost), MF_BYCOMMAND) != static_cast<UINT>(-1);
        const bool aboutExists =
            ::GetMenuState(systemMenu, static_cast<UINT>(systemmenu::kCommandAbout), MF_BYCOMMAND) != static_cast<UINT>(-1);

        if (topMostExists || aboutExists)
        {
            gConsoleMenuHost.RefreshState(systemMenu);
            gConsoleMenuInstalled.store(true);
            LogLine(L"[ConsoleMenuHook] System menu already contains custom commands. Refreshing state only.");
            return;
        }

        std::wstring errorMessage;
        if (!gConsoleMenuHost.Install(systemMenu, &errorMessage))
        {
            LogLine(L"[ConsoleMenuHook] Menu install failed: " + errorMessage);
            return;
        }

        ::DrawMenuBar(windowHandle);
        diagnosticconsole::LineBuilder line;
        line << L"[ConsoleMenuHook] Installed system menu items. count=" << ::GetMenuItemCount(systemMenu);
        LogLine(line.str());
        gConsoleMenuInstalled.store(true);
    }

    bool EnsureConsoleWindowIntegratedOnOwnerThread(HWND windowHandle)
    {
        if (gConsoleMenuInstalled.load())
        {
            gConsoleMenuHost.RefreshState(::GetSystemMenu(windowHandle, FALSE));
        }
        else
        {
            InstallConsoleWindowMenu(windowHandle);
        }

        if (gConsoleWindowSubclassed.load())
        {
            return true;
        }

        if (!::SetWindowSubclass(windowHandle, &ConsoleWindowSubclassProc, kConsoleMenuSubclassId, 0))
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] SetWindowSubclass failed on owner thread. gle=" << ::GetLastError();
            LogLine(line.str());
            return false;
        }

        LogLine(L"[ConsoleMenuHook] SetWindowSubclass succeeded on owner thread.");
        gConsoleWindowSubclassed.store(true);
        return true;
    }

    DWORD WINAPI WindowDiscoveryThreadProc(LPVOID)
    {
        for (;;)
        {
            if (gConsoleWindowSubclassed.load())
            {
                return 0;
            }
            if (gDiscoveryStopEvent != nullptr && ::WaitForSingleObject(gDiscoveryStopEvent, 0) == WAIT_OBJECT_0)
            {
                return 0;
            }

            if (HWND consoleWindow = FindConsoleWindowForCurrentProcess(); consoleWindow != nullptr)
            {
                CaptureConsoleWindow(consoleWindow);
                (void)::PostMessageW(consoleWindow, kConsoleHookIntegrateMsg, 0, 0);
            }

            const DWORD waitResult = gDiscoveryStopEvent != nullptr
                ? ::WaitForSingleObject(gDiscoveryStopEvent, 20)
                : WAIT_TIMEOUT;
            if (waitResult == WAIT_OBJECT_0)
            {
                return 0;
            }
        }
    }

    DWORD WINAPI InitializeConsoleMenuThread(LPVOID)
    {
        if (!InstallMessageTimingHooks())
        {
            LogLine(L"[ConsoleMenuHook] Hook installation failed.");
            return ERROR_PROC_NOT_FOUND;
        }

        gDiscoveryStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (gDiscoveryStopEvent != nullptr)
        {
            gDiscoveryThread = ::CreateThread(nullptr, 0, &WindowDiscoveryThreadProc, nullptr, 0, nullptr);
        }

        if (HWND consoleWindow = FindConsoleWindowForCurrentProcess(); consoleWindow != nullptr)
        {
            CaptureConsoleWindow(consoleWindow);
            (void)::PostMessageW(consoleWindow, kConsoleHookIntegrateMsg, 0, 0);
        }

        DWORD result = ERROR_TIMEOUT;
        for (int attempt = 0; attempt < 400; ++attempt)
        {
            HWND windowHandle = gConsoleWindow.load();
            if (windowHandle != nullptr && ::IsWindow(windowHandle) &&
                gConsoleMenuInstalled.load() && gConsoleWindowSubclassed.load())
            {
                result = 0;
                break;
            }

            ::Sleep(25);
        }

        if (gDiscoveryStopEvent != nullptr)
        {
            (void)::SetEvent(gDiscoveryStopEvent);
        }
        if (gDiscoveryThread != nullptr)
        {
            (void)::WaitForSingleObject(gDiscoveryThread, 2000);
            ::CloseHandle(gDiscoveryThread);
            gDiscoveryThread = nullptr;
        }
        if (gDiscoveryStopEvent != nullptr)
        {
            ::CloseHandle(gDiscoveryStopEvent);
            gDiscoveryStopEvent = nullptr;
        }

        RemoveMessageTimingHooks();
        if (result == ERROR_TIMEOUT)
        {
            LogLine(L"[ConsoleMenuHook] Timed out while waiting for captured console window.");
        }
        return result;
    }
}

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        ::DisableThreadLibraryCalls(moduleHandle);
        LogLine(L"[ConsoleMenuHook] DLL_PROCESS_ATTACH");
        gInitThread = ::CreateThread(nullptr, 0, &InitializeConsoleMenuThread, nullptr, 0, nullptr);
        break;

    case DLL_PROCESS_DETACH:
        RemoveMessageTimingHooks();
        if (gDiscoveryStopEvent != nullptr)
        {
            (void)::SetEvent(gDiscoveryStopEvent);
        }
        if (gDiscoveryThread != nullptr)
        {
            ::CloseHandle(gDiscoveryThread);
            gDiscoveryThread = nullptr;
        }
        if (gDiscoveryStopEvent != nullptr)
        {
            ::CloseHandle(gDiscoveryStopEvent);
            gDiscoveryStopEvent = nullptr;
        }
        if (gInitThread != nullptr)
        {
            ::CloseHandle(gInitThread);
            gInitThread = nullptr;
        }
        break;
    }

    return TRUE;
}
