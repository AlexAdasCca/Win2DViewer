#pragma once

namespace ConsoleMenuHook
{
    bool InstallMessageTimingHooks();
    void RemoveMessageTimingHooks();

    bool InstallMessageQuitHooks();
    void RemoveMessageQuitHooks();
} // namespace ConsoleMenuHook
