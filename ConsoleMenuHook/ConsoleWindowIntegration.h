#pragma once

#include "HookState.h"

namespace ConsoleMenuHook
{
    HWND FindConsoleWindowForCurrentProcess(bool verbose = false,
                                            const wchar_t* source = L"FindConsoleWindowForCurrentProcess");

    bool EnsureConsoleWindowIntegrated(HWND windowHandle);
    void TryIntegrateConsoleWindow(HWND windowHandle, std::wstring const& source);
    DWORD WINAPI WindowDiscoveryThreadProc(LPVOID);
    void RestoreOriginalWindowProcedureIfNeeded();
} // namespace ConsoleMenuHook
