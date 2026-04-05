#include "pch.h"

#include "ConsoleWindowIntegration.h"

#include <string_view>
#include <utility>

namespace ConsoleMenuHook
{
    namespace
    {
        struct WindowEnumContext
        {
            HWND FoundWindow = nullptr;
            DWORD ParentProcessId = 0;
            DWORD CurrentProcessId = 0;
            int ConsoleClassWindows = 0;
            int RejectedWindows = 0;
            int MatchedOwnerPid = 0;
            int MatchedSelfPid = 0;
            bool Verbose = false;
            const wchar_t* Source = L"";
        };

        void ShowAboutDialog(HWND ownerWindow)
        {
            ::MessageBoxW(
                ownerWindow,
                L"Win2DViewer Debug Console\nInjected into conhost for system menu extension.",
                L"Win2DViewer",
                MB_OK | MB_ICONINFORMATION);
        }

        BOOL CALLBACK FindConsoleWindowEnumProc(HWND windowHandle, LPARAM lParam)
        {
            auto* context = reinterpret_cast<WindowEnumContext*>(lParam);
            if (context == nullptr)
            {
                return TRUE;
            }

            wchar_t className[64]{};
            if (::GetClassNameW(windowHandle, className, _countof(className)) == 0)
            {
                return TRUE;
            }
            if (std::wstring_view(className) != L"ConsoleWindowClass")
            {
                return TRUE;
            }
            ++context->ConsoleClassWindows;

            DWORD processId = 0;
            (void)::GetWindowThreadProcessId(windowHandle, &processId);
            const bool matchesOwnerProcess = context->ParentProcessId != 0 && processId == context->ParentProcessId;
            const bool matchesConhostProcess = processId == context->CurrentProcessId;
            if (matchesOwnerProcess)
            {
                ++context->MatchedOwnerPid;
            }
            if (matchesConhostProcess)
            {
                ++context->MatchedSelfPid;
            }

            if (context->Verbose)
            {
                DiagnosticConsole::LineBuilder line;
                line << L"[ConsoleMenuHook] Enum candidate source=" << context->Source
                     << L" hwnd=0x" << std::hex << reinterpret_cast<ULONG_PTR>(windowHandle)
                     << L" pid=" << std::dec << processId
                     << L" parentPid=" << context->ParentProcessId
                     << L" selfPid=" << context->CurrentProcessId
                     << L" ownerMatch=" << (matchesOwnerProcess ? 1 : 0)
                     << L" selfMatch=" << (matchesConhostProcess ? 1 : 0);
                LogLine(line.str());
            }

            if (!matchesOwnerProcess && !matchesConhostProcess)
            {
                ++context->RejectedWindows;
                return TRUE;
            }

            context->FoundWindow = windowHandle;
            return FALSE;
        }

        bool EnsureConsoleMenuModel()
        {
            auto& runtimeState = GetRuntimeState();
            if (!runtimeState.ConsoleMenuHost.Items().empty())
            {
                return true;
            }

            SystemMenu::MenuItemSpec separator{};
            separator.separator = true;

            SystemMenu::MenuItemSpec topMostItem{};
            topMostItem.id = SystemMenu::kCommandWindowTopMost;
            topMostItem.text = L"窗口置顶";
            topMostItem.shortcut = SystemMenu::ShortcutBinding{};
            topMostItem.shortcut->virtualKey = 'T';
            topMostItem.shortcut->ctrl = true;
            topMostItem.shortcut->alt = true;
            topMostItem.onInvoke = [](HWND ownerWindow)
            {
                auto& state = GetRuntimeState();
                state.ConsoleWindowTopMost = !state.ConsoleWindowTopMost;
                ::SetWindowPos(
                    ownerWindow,
                    state.ConsoleWindowTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            };
            topMostItem.isChecked = []()
            { return GetRuntimeState().ConsoleWindowTopMost; };
            topMostItem.isEnabled = []()
            { return true; };

            SystemMenu::MenuItemSpec aboutItem{};
            aboutItem.id = SystemMenu::kCommandAbout;
            aboutItem.text = L"打开关于";
            aboutItem.shortcut = SystemMenu::ShortcutBinding{};
            aboutItem.shortcut->virtualKey = VK_F1;
            aboutItem.shortcut->alt = true;
            aboutItem.onInvoke = [](HWND ownerWindow)
            { ShowAboutDialog(ownerWindow); };
            aboutItem.isEnabled = []()
            { return true; };

            std::wstring errorMessage;
            if (!runtimeState.ConsoleMenuHost.AddItem(separator, &errorMessage) ||
                !runtimeState.ConsoleMenuHost.AddItem(std::move(topMostItem), &errorMessage) ||
                !runtimeState.ConsoleMenuHost.AddItem(std::move(aboutItem), &errorMessage))
            {
                LogLine(L"[ConsoleMenuHook] Failed to build menu model: " + errorMessage);
                return false;
            }

            return true;
        }

        LRESULT CALLBACK ConsoleWindowLongPtrProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
        {
            const auto callOriginal = [&]() -> LRESULT
            {
                auto& runtimeState = GetRuntimeState();
                const WNDPROC originalProc = runtimeState.ConsoleWindowOriginalWndProc.load();
                if (originalProc != nullptr && originalProc != &ConsoleWindowLongPtrProc)
                {
                    return ::CallWindowProcW(originalProc, windowHandle, message, wParam, lParam);
                }
                return ::DefWindowProcW(windowHandle, message, wParam, lParam);
            };

            auto& runtimeState = GetRuntimeState();
            switch (message)
            {
                case WM_INITMENUPOPUP:
                    runtimeState.ConsoleMenuHost.RefreshState(reinterpret_cast<HMENU>(wParam));
                    break;

                case WM_SYSCOMMAND:
                {
                    const UINT_PTR commandId = (wParam & 0xFFF0);
                    if (commandId == SC_CLOSE)
                    {
                        NotifyOwnerConsoleClose(L"LongPtr/SC_CLOSE");
                        return 0;
                    }
                    if (runtimeState.ConsoleMenuHost.HandleCommand(windowHandle, commandId))
                    {
                        DiagnosticConsole::LineBuilder line;
                        line << L"[ConsoleMenuHook] Handled longptr WM_SYSCOMMAND id=0x" << std::hex << commandId;
                        LogLine(line.str());
                        return 0;
                    }
                    break;
                }

                case WM_CLOSE:
                    NotifyOwnerConsoleClose(L"LongPtr/WM_CLOSE");
                    return 0;

                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    if (runtimeState.ConsoleMenuHost.HandleShortcut(windowHandle, message, wParam))
                    {
                        LogLine(L"[ConsoleMenuHook] Handled longptr shortcut.");
                        return 0;
                    }
                    break;

                case WM_NCDESTROY:
                {
                    NotifyOwnerConsoleClose(L"LongPtr/WM_NCDESTROY");
                    const WNDPROC originalProc = runtimeState.ConsoleWindowOriginalWndProc.load();
                    if (originalProc != nullptr && originalProc != &ConsoleWindowLongPtrProc)
                    {
                        (void)::SetWindowLongPtrW(windowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProc));
                    }
                    ResetWindowIntegrationStateAfterDestroy();

                    if (originalProc != nullptr && originalProc != &ConsoleWindowLongPtrProc)
                    {
                        return ::CallWindowProcW(originalProc, windowHandle, message, wParam, lParam);
                    }
                    return ::DefWindowProcW(windowHandle, message, wParam, lParam);
                }
            }

            return callOriginal();
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
                DiagnosticConsole::LineBuilder line;
                line << L"[ConsoleMenuHook] GetSystemMenu failed. gle=" << ::GetLastError();
                LogLine(line.str());
                return;
            }

            if (!EnsureConsoleMenuModel())
            {
                return;
            }

            auto& runtimeState = GetRuntimeState();
            const bool topMostExists =
                ::GetMenuState(systemMenu, static_cast<UINT>(SystemMenu::kCommandWindowTopMost), MF_BYCOMMAND) != static_cast<UINT>(-1);
            const bool aboutExists =
                ::GetMenuState(systemMenu, static_cast<UINT>(SystemMenu::kCommandAbout), MF_BYCOMMAND) != static_cast<UINT>(-1);

            if (topMostExists || aboutExists)
            {
                runtimeState.ConsoleMenuHost.RefreshState(systemMenu);
                runtimeState.ConsoleMenuInstalled.store(true);
                LogLine(L"[ConsoleMenuHook] System menu already contains custom commands. Refreshing state only.");
                return;
            }

            std::wstring errorMessage;
            if (!runtimeState.ConsoleMenuHost.Install(systemMenu, &errorMessage))
            {
                LogLine(L"[ConsoleMenuHook] Menu install failed: " + errorMessage);
                return;
            }

            ::DrawMenuBar(windowHandle);
            DiagnosticConsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Installed system menu items. count=" << ::GetMenuItemCount(systemMenu);
            LogLine(line.str());
            runtimeState.ConsoleMenuInstalled.store(true);
        }
    } // namespace

    HWND FindConsoleWindowForCurrentProcess(bool verbose, const wchar_t* source)
    {
        WindowEnumContext context{};
        context.ParentProcessId = QueryParentProcessId();
        context.CurrentProcessId = ::GetCurrentProcessId();
        context.Verbose = verbose;
        context.Source = source != nullptr ? source : L"";
        (void)::EnumWindows(&FindConsoleWindowEnumProc, reinterpret_cast<LPARAM>(&context));

        DiagnosticConsole::LineBuilder summary;
        summary << L"[ConsoleMenuHook] Enum summary source=" << context.Source
                << L" found=" << (context.FoundWindow != nullptr ? 1 : 0)
                << L" consoleClassCount=" << context.ConsoleClassWindows
                << L" rejected=" << context.RejectedWindows
                << L" ownerPidMatches=" << context.MatchedOwnerPid
                << L" selfPidMatches=" << context.MatchedSelfPid
                << L" parentPid=" << context.ParentProcessId
                << L" selfPid=" << context.CurrentProcessId;
        LogLine(summary.str());

        return context.FoundWindow;
    }

    bool EnsureConsoleWindowIntegrated(HWND windowHandle)
    {
        auto& runtimeState = GetRuntimeState();
        if (runtimeState.ConsoleMenuInstalled.load())
        {
            runtimeState.ConsoleMenuHost.RefreshState(::GetSystemMenu(windowHandle, FALSE));
        }
        else
        {
            InstallConsoleWindowMenu(windowHandle);
        }

        if (runtimeState.ConsoleWindowSubclassed.load())
        {
            return true;
        }

        // We intentionally keep a single subclassing path: SetWindowLongPtrW(GWLP_WNDPROC).
        //
        // Rationale:
        // 1) SetWindowSubclass cannot subclass windows across threads.
        //    Source (official):
        //    https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass
        // 2) In this injected conhost scenario, discovery and integration may run on worker threads.
        // 3) We validated that SetWindowLongPtrW is simpler and more stable in this code path.
        //    Source (official):
        //    https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwindowlongptrw
        // 4) Subclassing pitfalls discussion:
        //    https://stackoverflow.com/questions/1346582/why-does-this-window-subclassing-code-crash
        //
        // Important console-window caveat:
        // For ConsoleWindowClass, GetWindowThreadProcessId may report the console owner process
        // identity instead of the real UI thread that owns the HWND. Therefore, if we ever switch
        // back to SetWindowSubclass, do NOT rely on GetWindowThreadProcessId alone.
        //
        // Safer fallback pattern for SetWindowSubclass:
        // - If you own the target thread message loop, marshal to that thread and call SetWindowSubclass there.
        // - In our injected conhost scenario, we do not own the target window procedure contract, so an owner-thread
        //   hook bridge is safer:
        //     1) ownerTid = ResolveRealWindowThreadByEnumThreadWindows(hwnd);
        //     2) SetWindowsHookEx(WH_CALLWNDPROC or WH_GETMESSAGE, ..., ownerTid);
        //     3) PostMessage(hwnd, WM_NULL, 0, 0);
        //     4) In the hook callback (owner thread), call SetWindowSubclass(hwnd,...).
        ::SetLastError(0);
        const LONG_PTR previousProcValue = ::SetWindowLongPtrW(
            windowHandle,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&ConsoleWindowLongPtrProc));
        if (previousProcValue == 0 && ::GetLastError() != 0)
        {
            DiagnosticConsole::LineBuilder line;
            line << L"[ConsoleMenuHook] SetWindowLongPtrW(GWLP_WNDPROC) failed. gle=" << ::GetLastError();
            LogLine(line.str());
            return false;
        }

        const WNDPROC previousProc = reinterpret_cast<WNDPROC>(previousProcValue);
        if (previousProc != nullptr && previousProc != &ConsoleWindowLongPtrProc)
        {
            runtimeState.ConsoleWindowOriginalWndProc.store(previousProc);
        }

        LogLine(L"[ConsoleMenuHook] SetWindowLongPtrW(GWLP_WNDPROC) succeeded.");
        runtimeState.ConsoleWindowSubclassed.store(true);
        return true;
    }

    void TryIntegrateConsoleWindow(HWND windowHandle, std::wstring const& source)
    {
        if (windowHandle == nullptr || !::IsWindow(windowHandle))
        {
            return;
        }

        auto& runtimeState = GetRuntimeState();
        CaptureConsoleWindow(windowHandle);
        if (runtimeState.ConsoleWindow.load() != windowHandle || runtimeState.ConsoleWindowSubclassed.load())
        {
            return;
        }

        bool expected = false;
        if (!runtimeState.ConsoleIntegrateInProgress.compare_exchange_strong(expected, true))
        {
            return;
        }

        const bool integrated = EnsureConsoleWindowIntegrated(windowHandle);
        runtimeState.ConsoleIntegrateInProgress.store(false);

        if (integrated)
        {
            DiagnosticConsole::LineBuilder line;
            line << L"[ConsoleMenuHook] Integrated console window via " << source;
            LogLine(line.str());
        }
    }

    DWORD WINAPI WindowDiscoveryThreadProc(LPVOID)
    {
        auto& runtimeState = GetRuntimeState();
        constexpr DWORD kIntegrateWaitSliceMs = 10;
        constexpr DWORD kIntegrateWaitBudgetMs = 500;

        int attempt = 0;
        bool integrateWaitTimeoutLogged = false;
        for (;;)
        {
            ++attempt;
            if (runtimeState.ConsoleWindowSubclassed.load())
            {
                return 0;
            }
            if (runtimeState.DiscoveryStopEvent != nullptr && ::WaitForSingleObject(runtimeState.DiscoveryStopEvent, 0) == WAIT_OBJECT_0)
            {
                return 0;
            }

            if (runtimeState.ConsoleIntegrateInProgress.load())
            {
                DWORD waitedMs = 0;
                while (!runtimeState.ConsoleWindowSubclassed.load() &&
                       runtimeState.ConsoleIntegrateInProgress.load() &&
                       waitedMs < kIntegrateWaitBudgetMs)
                {
                    if (runtimeState.DiscoveryStopEvent != nullptr)
                    {
                        if (::WaitForSingleObject(runtimeState.DiscoveryStopEvent, kIntegrateWaitSliceMs) == WAIT_OBJECT_0)
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        ::Sleep(kIntegrateWaitSliceMs);
                    }
                    waitedMs += kIntegrateWaitSliceMs;
                }

                if (runtimeState.ConsoleWindowSubclassed.load())
                {
                    return 0;
                }

                if (!runtimeState.ConsoleIntegrateInProgress.load())
                {
                    integrateWaitTimeoutLogged = false;
                }
                else if (!integrateWaitTimeoutLogged)
                {
                    LogLine(L"[ConsoleMenuHook] Integration wait timed out; resuming window enumeration fallback.");
                    integrateWaitTimeoutLogged = true;
                }

                continue;
            }

            integrateWaitTimeoutLogged = false;
            const bool verbose = (attempt <= 3) || (attempt % 100 == 0);
            if (HWND consoleWindow = FindConsoleWindowForCurrentProcess(verbose, L"WindowDiscoveryThread"); consoleWindow != nullptr)
            {
                TryIntegrateConsoleWindow(consoleWindow, L"WindowDiscoveryThread");
                if (!runtimeState.ConsoleWindowSubclassed.load())
                {
                    // Wake the target window thread message loop to accelerate follow-up integration.
                    (void)::PostMessageW(consoleWindow, WM_NULL, 0, 0);
                }
            }

            const DWORD waitResult = runtimeState.DiscoveryStopEvent != nullptr
                                         ? ::WaitForSingleObject(runtimeState.DiscoveryStopEvent, 20)
                                         : WAIT_TIMEOUT;
            if (waitResult == WAIT_OBJECT_0)
            {
                return 0;
            }
        }
    }

    void RestoreOriginalWindowProcedureIfNeeded()
    {
        auto& runtimeState = GetRuntimeState();
        if (HWND windowHandle = runtimeState.ConsoleWindow.load(); windowHandle != nullptr && ::IsWindow(windowHandle))
        {
            const WNDPROC originalProc = runtimeState.ConsoleWindowOriginalWndProc.load();
            if (originalProc != nullptr && originalProc != &ConsoleWindowLongPtrProc)
            {
                (void)::SetWindowLongPtrW(windowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(originalProc));
            }
        }
        runtimeState.ConsoleWindowOriginalWndProc.store(nullptr);
    }
} // namespace ConsoleMenuHook
