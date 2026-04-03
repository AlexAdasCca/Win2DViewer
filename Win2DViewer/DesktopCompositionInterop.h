#pragma once

#include "pch.h"

#include <DispatcherQueue.h>
#include "WinrtNsAliases.h"

namespace desktopinterop
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
}
