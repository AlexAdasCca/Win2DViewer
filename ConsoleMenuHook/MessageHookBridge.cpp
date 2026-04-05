#include "pch.h"

#include "MessageHookBridge.h"

#include <detours/detours.h>

#include "ConsoleWindowIntegration.h"
#include "HookState.h"

namespace ConsoleMenuHook
{
    namespace
    {
        BOOL WINAPI HookedTranslateMessage(const MSG* message)
        {
            if (message != nullptr && message->hwnd != nullptr)
            {
                TryIntegrateConsoleWindow(message->hwnd, L"TranslateMessage");
            }

            auto& runtimeState = GetRuntimeState();
            return runtimeState.OriginalTranslateMessage != nullptr ? runtimeState.OriginalTranslateMessage(message) : FALSE;
        }

        LRESULT WINAPI HookedDispatchMessageW(const MSG* message)
        {
            auto& runtimeState = GetRuntimeState();
            if (message != nullptr && message->hwnd != nullptr)
            {
                TryIntegrateConsoleWindow(message->hwnd, L"DispatchMessageW");

                if (runtimeState.ConsoleWindow.load() == message->hwnd)
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

            return runtimeState.OriginalDispatchMessageW != nullptr ? runtimeState.OriginalDispatchMessageW(message) : 0;
        }

        BOOL WINAPI HookedGetMessageW(LPMSG message, HWND windowHandle, UINT minFilter, UINT maxFilter)
        {
            auto& runtimeState = GetRuntimeState();
            if (runtimeState.OriginalGetMessageW == nullptr)
            {
                return FALSE;
            }

            const BOOL result = runtimeState.OriginalGetMessageW(message, windowHandle, minFilter, maxFilter);
            if (result == 0)
            {
                NotifyOwnerConsoleClose(L"HookedGetMessageW/WM_QUIT");
            }

            return result;
        }

        BOOL WINAPI HookedPeekMessageW(LPMSG message, HWND windowHandle, UINT minFilter, UINT maxFilter, UINT removeMessage)
        {
            auto& runtimeState = GetRuntimeState();
            if (runtimeState.OriginalPeekMessageW == nullptr)
            {
                return FALSE;
            }

            const BOOL result = runtimeState.OriginalPeekMessageW(message, windowHandle, minFilter, maxFilter, removeMessage);
            if (result && message != nullptr && message->message == WM_QUIT)
            {
                NotifyOwnerConsoleClose(L"HookedPeekMessageW/WM_QUIT");
            }

            return result;
        }
    } // namespace

    bool InstallMessageTimingHooks()
    {
        auto& runtimeState = GetRuntimeState();
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

        status = DetourAttach(reinterpret_cast<PVOID*>(&runtimeState.OriginalTranslateMessage), HookedTranslateMessage);
        if (status == NO_ERROR)
        {
            status = DetourAttach(reinterpret_cast<PVOID*>(&runtimeState.OriginalDispatchMessageW), HookedDispatchMessageW);
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
        auto& runtimeState = GetRuntimeState();
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

        (void)DetourDetach(reinterpret_cast<PVOID*>(&runtimeState.OriginalDispatchMessageW), HookedDispatchMessageW);
        (void)DetourDetach(reinterpret_cast<PVOID*>(&runtimeState.OriginalTranslateMessage), HookedTranslateMessage);
        status = DetourTransactionCommit();
        if (status == NO_ERROR)
        {
            LogLine(L"[ConsoleMenuHook] Removed TranslateMessage/DispatchMessageW hooks.");
        }
    }

    bool InstallMessageQuitHooks()
    {
        auto& runtimeState = GetRuntimeState();
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

        status = DetourAttach(reinterpret_cast<PVOID*>(&runtimeState.OriginalGetMessageW), HookedGetMessageW);
        if (status == NO_ERROR)
        {
            status = DetourAttach(reinterpret_cast<PVOID*>(&runtimeState.OriginalPeekMessageW), HookedPeekMessageW);
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

        LogLine(L"[ConsoleMenuHook] Installed GetMessageW/PeekMessageW quit hooks.");
        return true;
    }

    void RemoveMessageQuitHooks()
    {
        auto& runtimeState = GetRuntimeState();
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

        (void)DetourDetach(reinterpret_cast<PVOID*>(&runtimeState.OriginalPeekMessageW), HookedPeekMessageW);
        (void)DetourDetach(reinterpret_cast<PVOID*>(&runtimeState.OriginalGetMessageW), HookedGetMessageW);
        status = DetourTransactionCommit();
        if (status == NO_ERROR)
        {
            LogLine(L"[ConsoleMenuHook] Removed GetMessageW/PeekMessageW quit hooks.");
        }
    }
} // namespace ConsoleMenuHook
