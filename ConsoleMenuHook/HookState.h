#pragma once

#include "pch.h"

#include <string>

namespace ConsoleMenuHook
{
    using TranslateMessageFn = BOOL(WINAPI*)(const MSG*);
    using DispatchMessageWFn = LRESULT(WINAPI*)(const MSG*);
    using GetMessageWFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
    using PeekMessageWFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

    struct RuntimeState
    {
        std::atomic<HWND> ConsoleWindow{nullptr};
        bool ConsoleWindowTopMost = false;
        HANDLE InitThread = nullptr;
        SystemMenu::MenuHost ConsoleMenuHost{L"Conhost.SystemMenu"};
        std::atomic_bool ConsoleMenuInstalled = false;
        std::atomic_bool ConsoleWindowSubclassed = false;
        std::atomic_bool ConsoleIntegrateInProgress = false;
        std::atomic<WNDPROC> ConsoleWindowOriginalWndProc{nullptr};
        std::atomic_bool ConsoleCloseNotified = false;
        HANDLE DiscoveryStopEvent = nullptr;
        HANDLE DiscoveryThread = nullptr;

        TranslateMessageFn OriginalTranslateMessage = ::TranslateMessage;
        DispatchMessageWFn OriginalDispatchMessageW = ::DispatchMessageW;
        GetMessageWFn OriginalGetMessageW = ::GetMessageW;
        PeekMessageWFn OriginalPeekMessageW = ::PeekMessageW;
    };

    RuntimeState& GetRuntimeState();

    void LogLine(std::wstring const& line);
    DWORD QueryParentProcessId();
    void NotifyOwnerConsoleClose(std::wstring const& source);
    void CaptureConsoleWindow(HWND windowHandle);
    void ResetWindowIntegrationStateAfterDestroy();
} // namespace ConsoleMenuHook
