#pragma once

#include <Windows.h>

namespace ConsoleMenuHook
{
    DWORD WINAPI InitializeConsoleMenuThread(LPVOID);
    void CleanupConsoleMenuHook();
}
