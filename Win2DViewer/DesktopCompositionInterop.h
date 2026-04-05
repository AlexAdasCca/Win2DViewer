#pragma once

#include "pch.h"

#include "WinrtNsAliases.h"
#include <DispatcherQueue.h>

namespace DesktopInterop
{
    enum class DesktopHostBackend
    {
        WinRTComposition = 0,
        DirectComposition = 1,
        WinRTHostBackdrop = 2,
    };

    wna::wd::sys::DispatcherQueueController CreateDispatcherQueueControllerForCurrentThread();
    wna::wd::sys::DispatcherQueueController EnsureDispatcherQueueControllerForCurrentThread();

    bool CreateDesktopHostTestWindow(DesktopHostBackend backend, std::wstring* errorMessage = nullptr);
    void ShowDesktopHostTestPanel(HWND ownerWindow);
} // namespace DesktopInterop
