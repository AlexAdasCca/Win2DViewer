#pragma once

#include "pch.h"

#include <string>
#include <wil/resource.h>

namespace ConsoleMenuHook
{
    using TranslateMessageFn = BOOL(WINAPI*)(const MSG*);
    using DispatchMessageWFn = LRESULT(WINAPI*)(const MSG*);
    using GetMessageWFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
    using PeekMessageWFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

    struct RuntimeState
    {
        // Window capture and integration state.
        std::atomic<HWND> ConsoleWindow{ nullptr };
        std::atomic_bool ConsoleWindowSubclassed = false;
        std::atomic_bool ConsoleIntegrateInProgress = false;
        std::atomic<WNDPROC> ConsoleWindowOriginalWndProc{ nullptr };
        std::atomic_bool ConsoleCloseNotified = false;

        // System menu model and UI state.
        SystemMenu::MenuHost ConsoleMenuHost{ L"Conhost.SystemMenu" };
        std::atomic_bool ConsoleMenuInstalled = false;
        bool ConsoleWindowTopMost = false;

        // Hook function pointers.
        TranslateMessageFn OriginalTranslateMessage = ::TranslateMessage;
        DispatchMessageWFn OriginalDispatchMessageW = ::DispatchMessageW;
        GetMessageWFn OriginalGetMessageW = ::GetMessageW;
        PeekMessageWFn OriginalPeekMessageW = ::PeekMessageW;

        // Lifecycle thread and event handles.
        wil::unique_handle InitThread;
        wil::unique_handle DiscoveryStopEvent;
        wil::unique_handle DiscoveryThread;
    };

    RuntimeState& GetRuntimeState();

    void LogLine(std::wstring const& line);
    DWORD QueryParentProcessId();
    void NotifyOwnerConsoleClose(std::wstring const& source);
    void CaptureConsoleWindow(HWND windowHandle);
    void ResetWindowIntegrationStateAfterDestroy();
} // namespace ConsoleMenuHook
