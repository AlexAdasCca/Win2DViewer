#include "pch.h"

#include <detours/detours.h>
#include <winnt.h>

namespace
{
    constexpr UINT_PTR kConsoleMenuSubclassId = 0x43534D48;

    using DefWindowProcWFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);

    std::atomic<HWND> gConsoleWindow{ nullptr };
    bool gConsoleWindowTopMost = false;
    HANDLE gInitThread = nullptr;
    systemmenu::MenuHost gConsoleMenuHost{ L"Conhost.SystemMenu" };
    std::atomic_bool gConsoleMenuInstalled = false;
    std::atomic_bool gConsoleWindowSubclassed = false;

    DefWindowProcWFn gOriginalDefWindowProcW = ::DefWindowProcW;

    void ShowAboutDialog(HWND ownerWindow);
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
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Captured console window hwnd=0x"
                 << std::hex << reinterpret_cast<ULONG_PTR>(windowHandle);
            LogLine(line.str());
        }
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

    bool EnsureConsoleWindowIntegratedOnOwnerThread(HWND windowHandle);

    LRESULT WINAPI HookedDefWindowProcW(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        LRESULT result = gOriginalDefWindowProcW != nullptr
            ? gOriginalDefWindowProcW(windowHandle, message, wParam, lParam)
            : 0;

        switch (message)
        {
        case WM_NCCREATE:
        case WM_CREATE:
        case WM_SHOWWINDOW:
        case WM_WINDOWPOSCHANGED:
        case WM_INITMENUPOPUP:
            CaptureConsoleWindow(windowHandle);
            if (gConsoleWindow.load() == windowHandle && !gConsoleWindowSubclassed.load())
            {
                (void)EnsureConsoleWindowIntegratedOnOwnerThread(windowHandle);
            }
            break;

        case WM_NCDESTROY:
            if (gConsoleWindow.load() == windowHandle)
            {
                gConsoleWindow.store(nullptr);
                gConsoleMenuInstalled.store(false);
                gConsoleWindowSubclassed.store(false);
            }
            break;
        }

        return result;
    }

    bool InstallDefWindowProcHook()
    {
        LONG status = DetourTransactionBegin();
        if (status != NO_ERROR)
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] DetourTransactionBegin failed. status=" << status;
            LogLine(line.str());
            return false;
        }

        status = DetourUpdateThread(::GetCurrentThread());
        if (status != NO_ERROR)
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] DetourUpdateThread failed. status=" << status;
            LogLine(line.str());
            (void)DetourTransactionAbort();
            return false;
        }

        status = DetourAttach(reinterpret_cast<PVOID*>(&gOriginalDefWindowProcW), HookedDefWindowProcW);
        if (status != NO_ERROR)
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] DetourAttach failed. status=" << status;
            LogLine(line.str());
            (void)DetourTransactionAbort();
            return false;
        }

        status = DetourTransactionCommit();
        if (status != NO_ERROR)
        {
            diagnosticconsole::LineBuilder line;
            line << L"[ConsoleMenuHook] DetourTransactionCommit failed. status=" << status;
            LogLine(line.str());
            return false;
        }

        LogLine(L"[ConsoleMenuHook] Installed DefWindowProcW detour.");
        return true;
    }

    void RemoveDefWindowProcHook()
    {
        if (gOriginalDefWindowProcW == nullptr)
        {
            return;
        }

        LONG status = DetourTransactionBegin();
        if (status != NO_ERROR)
        {
            return;
        }

        status = DetourUpdateThread(::GetCurrentThread());
        if (status == NO_ERROR)
        {
            status = DetourDetach(reinterpret_cast<PVOID*>(&gOriginalDefWindowProcW), HookedDefWindowProcW);
            if (status == NO_ERROR)
            {
                status = DetourTransactionCommit();
                if (status == NO_ERROR)
                {
                    LogLine(L"[ConsoleMenuHook] Removed DefWindowProcW detour.");
                    return;
                }
            }
        }

        (void)DetourTransactionAbort();
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
            if (gConsoleMenuHost.HandleCommand(windowHandle, commandId))
            {
                diagnosticconsole::LineBuilder line;
                line << L"[ConsoleMenuHook] Handled subclass WM_SYSCOMMAND id=0x" << std::hex << commandId;
                LogLine(line.str());
                return 0;
            }
            break;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (gConsoleMenuHost.HandleShortcut(windowHandle, message, wParam))
            {
                LogLine(L"[ConsoleMenuHook] Handled subclass shortcut.");
                return 0;
            }
            break;

        case WM_NCDESTROY:
            ::RemoveWindowSubclass(windowHandle, &ConsoleWindowSubclassProc, subclassId);
            gConsoleWindow.store(nullptr);
            gConsoleMenuInstalled.store(false);
            gConsoleWindowSubclassed.store(false);
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

    DWORD WINAPI InitializeConsoleMenuThread(LPVOID)
    {
        if (!InstallDefWindowProcHook())
        {
            LogLine(L"[ConsoleMenuHook] Hook installation failed.");
            return ERROR_PROC_NOT_FOUND;
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

        RemoveDefWindowProcHook();
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
        RemoveDefWindowProcHook();
        if (gInitThread != nullptr)
        {
            ::CloseHandle(gInitThread);
            gInitThread = nullptr;
        }
        break;
    }

    return TRUE;
}
