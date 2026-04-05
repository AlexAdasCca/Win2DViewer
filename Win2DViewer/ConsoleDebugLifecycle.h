#pragma once

#include <windows.h>

#include <string>

namespace ConsoleDebugLifecycle
{
    void SetStateSyncTargetWindow(HWND targetWindow);
    void EnsureDebugConsole();
    void ReleaseDebugConsole();
    void DebugPrintLine(std::wstring const& line);
} // namespace ConsoleDebugLifecycle
