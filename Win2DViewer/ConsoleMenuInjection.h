#pragma once

namespace ConsoleMenu
{
    void BeginConsoleHookInjectionAsync();
    bool EnsureConsoleHookInjected();
    void SetInjectionDiagnosticsEnabled(bool enabled) noexcept;
    void ResetConsoleHookState() noexcept;
}
