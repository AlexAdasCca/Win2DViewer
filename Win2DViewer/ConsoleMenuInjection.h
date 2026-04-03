#pragma once

namespace consolemenu
{
    void BeginConsoleHookInjectionAsync();
    bool EnsureConsoleHookInjected();
    void SetInjectionDiagnosticsEnabled(bool enabled) noexcept;
    void ResetConsoleHookState() noexcept;
}
