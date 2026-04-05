#include "pch.h"

#include "HookLifecycle.h"
#include "HookState.h"

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID)
{
    auto& runtimeState = ConsoleMenuHook::GetRuntimeState();
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            ::DisableThreadLibraryCalls(moduleHandle);
            ConsoleMenuHook::LogLine(L"[ConsoleMenuHook] DLL_PROCESS_ATTACH");
            runtimeState.InitThread = ::CreateThread(nullptr, 0, &ConsoleMenuHook::InitializeConsoleMenuThread, nullptr, 0, nullptr);
            break;

        case DLL_PROCESS_DETACH:
            ConsoleMenuHook::CleanupConsoleMenuHook();
            break;
    }

    return TRUE;
}
